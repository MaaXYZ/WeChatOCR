#include <WeChatOCR/MaaAdapter.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

struct WcoEngine {};
struct WcoResult {};

namespace {
using Json = nlohmann::json;
std::atomic<WcoStatus> mock_status = WCO_OK;
std::atomic<bool> mock_confidence = true;
std::atomic<int> mock_calls = 0;

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct Outcome {
    bool hit;
    MaaRect box;
    Json detail;
};

Outcome run(MaaTasker* tasker, MaaImageBuffer* image, Json params, int index = 0)
{
    const auto input = Json { { "custom_recognition", "WeChatOCR" }, { "roi", { 10, 20, 40, 30 } },
                              { "index", index }, { "custom_recognition_param", std::move(params) } }.dump();
    const auto id = MaaTaskerPostRecognition(tasker, "Custom", input.c_str(), image);
    require(id != MaaInvalidId, "cannot enqueue recognition");
    require(MaaTaskerWait(tasker, id) == MaaStatus_Succeeded, "recognition task failed");
    MaaSize count = 1;
    MaaNodeId node_id = MaaInvalidId;
    MaaStatus status;
    require(MaaTaskerGetTaskDetail(tasker, id, nullptr, &node_id, &count, &status) && count == 1,
            "missing task result");
    MaaRecoId reco_id = MaaInvalidId;
    require(MaaTaskerGetNodeDetail(tasker, node_id, nullptr, &reco_id, nullptr, nullptr), "missing node result");
    const std::unique_ptr<MaaStringBuffer, decltype(&MaaStringBufferDestroy)>
        detail(MaaStringBufferCreate(), MaaStringBufferDestroy);
    Outcome result {};
    MaaBool hit = false;
    require(MaaTaskerGetRecognitionDetail(tasker, reco_id, nullptr, nullptr, &hit, &result.box,
                                         detail.get(), nullptr, nullptr), "missing recognition result");
    result.hit = hit;
    result.detail = Json::parse(MaaStringBufferGet(detail.get()));
    return result;
}
} // namespace

extern "C" {
WcoStatus WCO_CALL wco_recognize(WcoEngine*, const WcoImageView* image, uint32_t timeout_ms, WcoResult** result)
{
    ++mock_calls;
    require(image && image->format == WCO_PIXEL_BGR, "incorrect image format");
    require(image->width == 40 && image->height == 30 && image->stride == 300,
            "ROI dimensions or non-contiguous row stride lost");
    require(image->data[0] == 10 && image->data[1] == 20 && image->data[2] == 5,
            "ROI pixels differ from source");
    require(timeout_ms > 0, "timeout lost");
    *result = nullptr;
    if (mock_status != WCO_OK) {
        return mock_status;
    }
    *result = new WcoResult;
    return WCO_OK;
}
const char* WCO_CALL wco_last_error(void) { return "mock timeout"; }
uint32_t WCO_CALL wco_result_line_count(const WcoResult*) { return 2; }
WcoStatus WCO_CALL wco_result_get_line(const WcoResult*, uint32_t index, WcoLine* line)
{
    const float first[] = { 1, 2, 11, 2, 11, 12, 1, 12 };
    const float second[] = { 15, 3, 25, 3, 25, 13, 15, 13 };
    std::copy_n(index == 0 ? first : second, 8, line->quad);
    line->text = index == 0 ? " 開始 " : "结束";
    line->text_length = std::char_traits<char>::length(line->text);
    line->confidence = 0.8f;
    line->has_confidence = mock_confidence;
    return WCO_OK;
}
void WCO_CALL wco_result_destroy(WcoResult* result) { delete result; }
}

int main()
{
    try {
        MaaLoggingLevel log_level = MaaLoggingLevel_Off;
        MaaGlobalSetOption(MaaGlobalOption_StdoutLevel, &log_level, sizeof(log_level));
        const std::unique_ptr<MaaResource, decltype(&MaaResourceDestroy)> resource(MaaResourceCreate(), MaaResourceDestroy);
        const std::unique_ptr<MaaTasker, decltype(&MaaTaskerDestroy)> tasker(MaaTaskerCreate(), MaaTaskerDestroy);
        const std::unique_ptr<MaaImageBuffer, decltype(&MaaImageBufferDestroy)> image(MaaImageBufferCreate(), MaaImageBufferDestroy);
        require(resource && tasker && image, "cannot create Maa objects");
        require(MaaTaskerBindResource(tasker.get(), resource.get()), "cannot bind resource");
        std::vector<uint8_t> pixels(100 * 80 * 3);
        for (int y = 0; y < 80; ++y) {
            for (int x = 0; x < 100; ++x) {
                const auto offset = static_cast<size_t>(y * 100 + x) * 3;
                pixels[offset] = static_cast<uint8_t>(x);
                pixels[offset + 1] = static_cast<uint8_t>(y);
                pixels[offset + 2] = 5;
            }
        }
        require(MaaImageBufferSetRawData(image.get(), pixels.data(), 100, 80, 16), "cannot create BGR image");
        WcoEngine engine;
        WcoMaaRegistration* registration = nullptr;
        require(wco_maa_register(resource.get(), &engine, nullptr, &registration) == WCO_OK, "registration failed");
        WcoMaaRegistration* duplicate = nullptr;
        require(wco_maa_register(resource.get(), &engine, nullptr, &duplicate) == WCO_INVALID_ARGUMENT && !duplicate,
                "duplicate name overwrote registration");

        const auto result = run(tasker.get(), image.get(),
            { { "expected", "^开.$" }, { "replace", { { "開", "开" } } }, { "threshold", 0.7 }, { "timeout_ms", 900 } });
        require(result.hit, "Unicode regex/replacement did not match");
        require(result.box.x == 11 && result.box.y == 22 && result.box.width == 10 && result.box.height == 10,
                "ROI coordinates were not restored");
        require(result.detail.at("all").size() == 2 && result.detail.at("filtered").size() == 1,
                "Maa multi-result protocol was not parsed");
        require(result.detail.at("best").at("detail").at("text") == "开始", "replacement result differs");

        const auto last = run(tasker.get(), image.get(), Json::object(), -1);
        require(last.hit && last.box.x == 25 && last.box.y == 23, "framework negative index failed");
        mock_confidence = false;
        require(!run(tasker.get(), image.get(), { { "threshold", 0.1 } }).hit, "unknown confidence passed positive threshold");
        const auto unknown = run(tasker.get(), image.get(), Json::object());
        require(unknown.hit && unknown.detail.at("best").at("detail").at("confidence").is_null(),
                "unknown confidence was synthesized");
        mock_status = WCO_TIMEOUT;
        const auto timeout = run(tasker.get(), image.get(), Json::object());
        require(!timeout.hit && timeout.detail.at("all").at(0).at("detail").at("status") == WCO_TIMEOUT,
                "operational failure not preserved in recognition detail");
        const auto calls = mock_calls.load();
        const auto bad_params = run(tasker.get(), image.get(), { { "only_rec", true } });
        require(!bad_params.hit && mock_calls == calls, "unsupported parameter reached OCR");
        require(wco_maa_unregister(registration) == WCO_OK, "unregister failed");
        std::cout << "Maa adapter contract tests passed\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
