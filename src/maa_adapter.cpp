#include <WeChatOCR/MaaAdapter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <codecvt>
#include <cwctype>
#include <limits>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/regex.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

struct WcoMaaRegistration {
    MaaResource* resource;
    WcoEngine* engine;
    std::string name;
};

namespace {

using Json = nlohmann::json;
using WideCodec = std::conditional_t<sizeof(wchar_t) == 2,
                                    std::codecvt_utf8_utf16<wchar_t>,
                                    std::codecvt_utf8<wchar_t>>;
thread_local std::string last_error;

struct Params {
    uint32_t timeout_ms = 10000;
    double threshold = 0;
    std::string order_by = "Horizontal";
    std::vector<boost::wregex> expected;
    std::vector<std::pair<boost::wregex, std::wstring>> replace;
};

struct Line {
    cv::Rect box;
    std::array<float, 8> quad;
    std::wstring text;
    double confidence;
    bool has_confidence;
};

std::wstring to_wide(const std::string& value)
{
    return std::wstring_convert<WideCodec>().from_bytes(value);
}

std::string to_utf8(const std::wstring& value)
{
    return std::wstring_convert<WideCodec>().to_bytes(value);
}

Params parse_params(const char* input)
{
    const auto json = Json::parse(input ? input : "{}");
    if (!json.is_object()) {
        throw std::invalid_argument("custom_recognition_param must be an object");
    }
    const std::vector<std::string> supported { "timeout_ms", "threshold", "expected", "replace", "order_by" };
    for (const auto& [key, value] : json.items()) {
        if (std::find(supported.begin(), supported.end(), key) == supported.end()) {
            throw std::invalid_argument("unsupported WeChatOCR parameter: " + key);
        }
    }

    Params params;
    if (json.contains("timeout_ms")) {
        if (!json.at("timeout_ms").is_number_integer()) {
            throw std::invalid_argument("timeout_ms must be a positive integer");
        }
        const auto timeout = json.at("timeout_ms").get<int64_t>();
        if (timeout <= 0 || static_cast<uint64_t>(timeout) > UINT32_MAX) {
            throw std::invalid_argument("timeout_ms is outside uint32 range");
        }
        params.timeout_ms = static_cast<uint32_t>(timeout);
    }
    params.threshold = json.value("threshold", 0.0);
    if (!std::isfinite(params.threshold) || params.threshold < 0) {
        throw std::invalid_argument("threshold must be a finite nonnegative number");
    }
    params.order_by = json.value("order_by", std::string("Horizontal"));
    const std::vector<std::string> orders { "Horizontal", "Vertical", "Area", "Length", "Expected" };
    if (std::find(orders.begin(), orders.end(), params.order_by) == orders.end()) {
        throw std::invalid_argument("unsupported order_by");
    }
    if (json.contains("expected")) {
        auto expected = json.at("expected");
        if (expected.is_string()) {
            expected = Json::array({ expected });
        }
        for (const auto& pattern : expected.get<std::vector<std::string>>()) {
            params.expected.emplace_back(to_wide(pattern));
        }
    }
    if (json.contains("replace")) {
        for (const auto& pair : json.at("replace").get<std::vector<std::vector<std::string>>>()) {
            if (pair.size() != 2) {
                throw std::invalid_argument("replace entries must contain [regex, replacement]");
            }
            params.replace.emplace_back(boost::wregex(to_wide(pair[0])), to_wide(pair[1]));
        }
    }
    return params;
}

cv::Mat decode_image(const MaaImageBuffer* image)
{
    // Maa's public raw-image API has no stride accessor. Decoding its cached PNG
    // preserves correctness when the underlying cv::Mat is a non-contiguous ROI.
    auto* data = MaaImageBufferGetEncoded(image);
    const auto size = MaaImageBufferGetEncodedSize(image);
    if (!data || !size || size > static_cast<MaaSize>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("image has no valid encoded representation");
    }
    auto decoded = cv::imdecode(cv::Mat(1, static_cast<int>(size), CV_8UC1, data), cv::IMREAD_COLOR);
    if (decoded.empty()) {
        throw std::invalid_argument("image decoding failed");
    }
    return decoded;
}

cv::Rect checked_roi(const MaaRect* roi, const cv::Mat& image)
{
    if (!roi || roi->x < 0 || roi->y < 0 || roi->width <= 0 || roi->height <= 0
        || static_cast<int64_t>(roi->x) + roi->width > image.cols
        || static_cast<int64_t>(roi->y) + roi->height > image.rows) {
        throw std::invalid_argument("ROI is outside the image");
    }
    return { roi->x, roi->y, roi->width, roi->height };
}

Line make_line(const WcoLine& raw, const cv::Rect& roi)
{
    if ((!raw.text && raw.text_length) || raw.text_length > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error("OCR returned an invalid text buffer");
    }
    Line line { {}, {}, to_wide(std::string(raw.text ? raw.text : "", raw.text_length)),
                raw.confidence, raw.has_confidence != 0 };
    float left = static_cast<float>(roi.width);
    float right = 0;
    float top = static_cast<float>(roi.height);
    float bottom = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(raw.quad[i * 2]) || !std::isfinite(raw.quad[i * 2 + 1])) {
            throw std::runtime_error("OCR returned a non-finite coordinate");
        }
        const auto x = std::clamp(raw.quad[i * 2], 0.0f, static_cast<float>(roi.width));
        const auto y = std::clamp(raw.quad[i * 2 + 1], 0.0f, static_cast<float>(roi.height));
        line.quad[i * 2] = x + roi.x;
        line.quad[i * 2 + 1] = y + roi.y;
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
    const auto x = static_cast<int>(std::floor(left));
    const auto y = static_cast<int>(std::floor(top));
    line.box = { roi.x + x, roi.y + y, static_cast<int>(std::ceil(right)) - x,
                 static_cast<int>(std::ceil(bottom)) - y };
    return line;
}

size_t expected_index(const Line& line, const Params& params)
{
    for (size_t i = 0; i < params.expected.size(); ++i) {
        if (boost::regex_search(line.text, params.expected[i])) {
            return i;
        }
    }
    return params.expected.size();
}

bool filter_line(Line& line, const Params& params)
{
    if (params.threshold > 0
        && (!line.has_confidence || !std::isfinite(line.confidence) || line.confidence < params.threshold)) {
        return false;
    }
    const auto first = std::find_if_not(line.text.begin(), line.text.end(), [](wchar_t ch) { return std::iswspace(ch); });
    const auto last = std::find_if_not(line.text.rbegin(), line.text.rend(), [](wchar_t ch) { return std::iswspace(ch); }).base();
    line.text = first < last ? std::wstring(first, last) : std::wstring();
    for (const auto& [pattern, replacement] : params.replace) {
        line.text = boost::regex_replace(line.text, pattern, replacement);
    }
    return params.expected.empty() || expected_index(line, params) < params.expected.size();
}

void sort_lines(std::vector<Line>& lines, const Params& params)
{
    std::stable_sort(lines.begin(), lines.end(), [&](const Line& lhs, const Line& rhs) {
        if (params.order_by == "Vertical") {
            return lhs.box.y == rhs.box.y ? lhs.box.x < rhs.box.x : lhs.box.y < rhs.box.y;
        }
        if (params.order_by == "Area") {
            return static_cast<int64_t>(lhs.box.width) * lhs.box.height
                   > static_cast<int64_t>(rhs.box.width) * rhs.box.height;
        }
        if (params.order_by == "Length") {
            return lhs.text.size() > rhs.text.size();
        }
        if (params.order_by == "Expected") {
            return expected_index(lhs, params) < expected_index(rhs, params);
        }
        return lhs.box.x == rhs.box.x ? lhs.box.y < rhs.box.y : lhs.box.x < rhs.box.x;
    });
}

Json serialize_lines(const std::vector<Line>& lines)
{
    auto json = Json::array();
    for (const auto& line : lines) {
        json.push_back({ { "box", { line.box.x, line.box.y, line.box.width, line.box.height } },
                         { "detail", { { "text", to_utf8(line.text) }, { "quad", line.quad },
                                       { "confidence", line.has_confidence ? Json(line.confidence) : Json(nullptr) },
                                       { "has_confidence", line.has_confidence } } } });
    }
    return json;
}

MaaBool callback_error(MaaStringBuffer* detail, WcoStatus status, const std::string& message) noexcept
{
    try {
        last_error = message;
        const auto json = Json { { "status", status }, { "error", message } }.dump();
        MaaStringBufferSet(detail, json.c_str());
    }
    catch (...) {
        if (detail) {
            MaaStringBufferSet(detail, "{\"status\":10,\"error\":\"adapter allocation failed\"}");
        }
    }
    return false;
}

MaaBool MAA_CALL recognize(MaaContext*, MaaTaskId, const char*, const char*, const char* custom_params,
                           const MaaImageBuffer* image, const MaaRect* roi, void* opaque,
                           MaaRect* out_box, MaaStringBuffer* out_detail)
{
    try {
        if (!opaque || !image || !out_box || !out_detail) {
            return false;
        }
        MaaRectSet(out_box, 0, 0, 0, 0);
        last_error.clear();
        const auto params = parse_params(custom_params);
        const auto decoded = decode_image(image);
        const auto region = checked_roi(roi, decoded);
        const auto cropped = decoded(region);
        WcoImageView view {};
        view.struct_size = sizeof(view);
        view.format = WCO_PIXEL_BGR;
        view.data = cropped.data;
        view.data_size = (cropped.rows - 1) * cropped.step + cropped.cols * cropped.elemSize();
        view.width = static_cast<uint32_t>(cropped.cols);
        view.height = static_cast<uint32_t>(cropped.rows);
        view.stride = cropped.step;

        WcoResult* raw_result = nullptr;
        const auto status = wco_recognize(static_cast<WcoMaaRegistration*>(opaque)->engine,
                                          &view, params.timeout_ms, &raw_result);
        const std::unique_ptr<WcoResult, decltype(&wco_result_destroy)> result(raw_result, wco_result_destroy);
        if (status != WCO_OK) {
            return callback_error(out_detail, status, wco_last_error());
        }
        if (!result) {
            return callback_error(out_detail, WCO_INTERNAL_ERROR, "SDK returned success without a result");
        }
        std::vector<Line> all;
        std::vector<Line> filtered;
        for (uint32_t i = 0; i < wco_result_line_count(result.get()); ++i) {
            WcoLine raw {};
            raw.struct_size = sizeof(raw);
            const auto line_status = wco_result_get_line(result.get(), i, &raw);
            if (line_status != WCO_OK) {
                return callback_error(out_detail, line_status, wco_last_error());
            }
            auto line = make_line(raw, region);
            all.emplace_back(line);
            if (filter_line(line, params)) {
                filtered.emplace_back(std::move(line));
            }
        }
        sort_lines(all, params);
        sort_lines(filtered, params);
        const auto json = Json { { "$all", serialize_lines(all) }, { "$filtered", serialize_lines(filtered) },
                                 { "detail", { { "engine", "WeChatOCR" }, { "status", WCO_OK } } } }.dump();
        return MaaStringBufferSet(out_detail, json.c_str());
    }
    catch (const boost::regex_error& error) {
        return callback_error(out_detail, WCO_INVALID_ARGUMENT, error.what());
    }
    catch (const Json::exception& error) {
        return callback_error(out_detail, WCO_INVALID_ARGUMENT, error.what());
    }
    catch (const std::invalid_argument& error) {
        return callback_error(out_detail, WCO_INVALID_ARGUMENT, error.what());
    }
    catch (const std::bad_alloc&) {
        return callback_error(out_detail, WCO_OUT_OF_MEMORY, "adapter allocation failed");
    }
    catch (const std::exception& error) {
        return callback_error(out_detail, WCO_INTERNAL_ERROR, error.what());
    }
    catch (...) {
        return false;
    }
}

bool name_exists(MaaResource* resource, const std::string& name)
{
    const std::unique_ptr<MaaStringListBuffer, decltype(&MaaStringListBufferDestroy)>
        names(MaaStringListBufferCreate(), MaaStringListBufferDestroy);
    if (!names || !MaaResourceGetCustomRecognitionList(resource, names.get())) {
        throw std::runtime_error("cannot query registered Maa recognitions");
    }
    for (MaaSize i = 0; i < MaaStringListBufferSize(names.get()); ++i) {
        const auto* entry = MaaStringListBufferAt(names.get(), i);
        const auto* text = entry ? MaaStringBufferGet(entry) : nullptr;
        if (text && name == text) {
            return true;
        }
    }
    return false;
}

} // namespace

WcoStatus WCO_CALL wco_maa_register(MaaResource* resource, WcoEngine* engine,
                                  const char* name, WcoMaaRegistration** registration)
{
    if (registration) {
        *registration = nullptr;
    }
    if (!resource || !engine || !registration || (name && !*name)) {
        last_error = "resource, engine, output and a nonempty name are required";
        return WCO_INVALID_ARGUMENT;
    }
    try {
        last_error.clear();
        auto result = std::make_unique<WcoMaaRegistration>(WcoMaaRegistration { resource, engine, name ? name : "WeChatOCR" });
        if (name_exists(resource, result->name)) {
            last_error = "a recognition with this name is already registered";
            return WCO_INVALID_ARGUMENT;
        }
        if (!MaaResourceRegisterCustomRecognition(resource, result->name.c_str(), recognize, result.get())) {
            last_error = "MaaResourceRegisterCustomRecognition failed";
            return WCO_INTERNAL_ERROR;
        }
        *registration = result.release();
        return WCO_OK;
    }
    catch (const std::bad_alloc&) {
        return WCO_OUT_OF_MEMORY;
    }
    catch (const std::exception& error) {
        last_error = error.what();
        return WCO_INTERNAL_ERROR;
    }
    catch (...) {
        return WCO_INTERNAL_ERROR;
    }
}

WcoStatus WCO_CALL wco_maa_unregister(WcoMaaRegistration* registration)
{
    if (!registration) {
        return WCO_INVALID_ARGUMENT;
    }
    const std::unique_ptr<WcoMaaRegistration> owned(registration);
    if (!MaaResourceUnregisterCustomRecognition(owned->resource, owned->name.c_str())) {
        last_error = "MaaResourceUnregisterCustomRecognition failed";
        return WCO_INTERNAL_ERROR;
    }
    last_error.clear();
    return WCO_OK;
}

const char* WCO_CALL wco_maa_last_error(void)
{
    return last_error.c_str();
}
