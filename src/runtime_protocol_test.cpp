// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime_protocol.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

void check(bool condition, const char* description)
{
    if (!condition)
        throw std::runtime_error(description);
}

std::vector<uint8_t> hex(const char* text)
{
    std::istringstream stream(text);
    std::vector<uint8_t> output;
    unsigned value = 0;
    while (stream >> std::hex >> value) {
        check(value <= 255, "Invalid fixture byte");
        output.push_back(static_cast<uint8_t>(value));
    }
    return output;
}

wco::protocol::Message decode(uint32_t version, uint32_t request_id,
                              const std::vector<uint8_t>& data)
{
    wco::protocol::Message message;
    std::string error;
    const bool success = wco::protocol::decode(version, request_id, data.data(), data.size(), message, error);
    check(success,
          error.empty() ? "Fixture could not be decoded" : error.c_str());
    return message;
}

void fixtures()
{
    using wco::protocol::Message;
    check(wco::protocol::make_request(3, 2, "a.png")
          == hex("10 02 1a 07 0a 05 61 2e 70 6e 67"), "WeChat 3 request bytes");
    check(wco::protocol::make_request(4, 2, "a.png")
          == hex("08 02 12 05 61 2e 70 6e 67 32 06 08 01 10 01 18 00"),
          "WeChat 4 request bytes");

    const auto hello3 = decode(3, 1, hex("08 01 10 01"));
    check(hello3.kind == Message::Kind::ready && hello3.supported, "WeChat 3 handshake");
    const auto hello4 = decode(4, 10001, hex("08 01"));
    check(hello4.kind == Message::Kind::ready && hello4.supported, "WeChat 4 handshake");
    check(!decode(4, 10001, {}).supported, "Missing supported flag must fail closed");

    // A hand-authored WeChat 3 response: task 2, one line, .5 score,
    // axis-aligned [1,2,3,4], image size 5x6. Scalar defaults are omitted.
    const auto fixture3 = hex(
        "10 02 22 22 0a 1c 12 01 78 1d 00 00 00 3f "
        "2d 00 00 80 3f 35 00 00 00 40 3d 00 00 40 40 45 00 00 80 40 "
        "10 05 18 06");
    const auto result3 = decode(3, 1, fixture3);
    check(result3.kind == Message::Kind::result && result3.task_id == 2, "WeChat 3 task ID");
    check(result3.result.width == 5 && result3.result.height == 6, "WeChat 3 dimensions");
    check(result3.result.vendor_error == 0 && result3.result.lines.size() == 1, "WeChat 3 result");
    const auto& line3 = result3.result.lines.front();
    check(line3.text == "x" && line3.has_score && line3.score == .5f, "WeChat 3 text and score");
    const float expected3[] { 1, 2, 3, 2, 3, 4, 1, 4 };
    for (size_t i = 0; i < 8; ++i)
        check(line3.quad[i] == expected3[i], "Bounding box fallback");

    // WeChat 4 response with an actual rotated quadrilateral. Its corners must
    // survive independently of the axis-aligned coordinate fallback.
    const auto fixture4 = hex(
        "08 02 1a 42 1a 3c 0a 30 "
        "0a 0a 0d 00 00 80 3f 15 00 00 00 40 "
        "12 0a 0d 00 00 30 41 15 00 00 40 40 "
        "1a 0a 0d 00 00 20 41 15 00 00 40 41 "
        "22 0a 0d 00 00 00 00 15 00 00 30 41 "
        "12 03 61 62 63 1d 00 00 40 3f 20 14 28 1e");
    const auto result4 = decode(4, 10011, fixture4);
    check(result4.task_id == 2 && result4.result.width == 20 && result4.result.height == 30,
          "WeChat 4 metadata");
    check(result4.result.lines.size() == 1, "WeChat 4 line count");
    const auto& line4 = result4.result.lines.front();
    check(line4.text == "abc" && line4.has_score && line4.score == .75f, "WeChat 4 text and score");
    const float expected4[] { 1, 2, 11, 3, 10, 12, 0, 11 };
    for (size_t i = 0; i < 8; ++i)
        check(line4.quad[i] == expected4[i], "Rotated quad preservation");

    const auto error3 = decode(3, 1, hex("10 02 18 f9 ff ff ff ff ff ff ff ff 01"));
    check(error3.result.vendor_error == -7 && error3.result.lines.empty(), "Signed vendor error");
    check(decode(4, 10011, hex("08 02 1a 00")).result.lines.empty(), "Empty successful OCR result");
    check(decode(4, 12345, hex("ff")).kind == Message::Kind::ignored, "Unknown request IDs");

    auto extended = fixture4;
    const auto unknown = hex("f8 07 7b 82 08 03 61 62 63 89 08 01 02 03 04 05 06 07 08");
    extended.insert(extended.end(), unknown.begin(), unknown.end());
    check(decode(4, 10011, extended).result.lines.front().text == "abc", "Unknown fields skipped");

    const std::vector<std::vector<uint8_t>> malformed {
        hex("08"),                            // Truncated varint.
        hex("00"),                            // Field number zero.
        hex("08 ff ff ff ff ff ff ff ff ff 02"), // Varint overflow.
        hex("1a 7f 00"),                      // Truncated length-delimited value.
        hex("08 02 1a 02 1b 00"),             // Unsupported group wire type.
        hex("08 02"),                         // Missing result and error.
        hex("08 02 1a 04 20 ff ff ff"),       // Truncated nested scalar.
        hex("08 02 1a 08 1a 06 1d 00 00 c0 7f 00") // Non-finite score.
    };
    for (const auto& item : malformed) {
        Message message;
        std::string error;
        check(!wco::protocol::decode(4, 10011, item.data(), item.size(), message, error),
              "Malformed response accepted");
        check(!error.empty() && message.kind == Message::Kind::ignored, "Parse error contract");
    }
    Message message;
    std::string error;
    check(!wco::protocol::decode(4, 10011, nullptr, 1, message, error), "Null payload rejected");
    const uint8_t byte = 0;
    check(!wco::protocol::decode(4, 10011, &byte,
          wco::protocol::max_message_bytes + 1, message, error), "Oversize payload rejected");
    for (uint64_t id : { uint64_t(0), uint64_t(1), uint64_t(0x80000000) }) {
        bool rejected = false;
        try { (void)wco::protocol::make_request(3, id, "x.png"); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid vendor task ID rejected");
    }
}

} // namespace

int main()
{
    try {
        fixtures();
        std::cout << "Runtime protocol fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
