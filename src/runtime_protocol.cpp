// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime_protocol.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace wco::protocol {
namespace {

struct Field {
    uint32_t number = 0;
    uint32_t wire = 0;
    uint64_t integer = 0;
    const uint8_t* bytes = nullptr;
    size_t size = 0;
};

class Reader {
public:
    Reader(const void* data, size_t size)
        : current_(static_cast<const uint8_t*>(data)), remaining_(size) {}

    bool done() const { return remaining_ == 0; }

    bool next(Field& field)
    {
        field = {};
        uint64_t tag = 0;
        if (!varint(tag) || (tag >> 3) == 0 || (tag >> 3) > 0x1fffffff)
            return false;
        field.number = static_cast<uint32_t>(tag >> 3);
        field.wire = static_cast<uint32_t>(tag & 7);
        switch (field.wire) {
        case 0:
            return varint(field.integer);
        case 1:
            return take(8, field);
        case 2: {
            uint64_t length = 0;
            if (!varint(length) || length > remaining_)
                return false;
            return take(static_cast<size_t>(length), field);
        }
        case 5:
            return take(4, field);
        default:
            // Deprecated groups are not part of this protocol.
            return false;
        }
    }

private:
    bool take(size_t length, Field& field)
    {
        if (length > remaining_)
            return false;
        field.bytes = current_;
        field.size = length;
        current_ += length;
        remaining_ -= length;
        return true;
    }

    bool varint(uint64_t& value)
    {
        value = 0;
        for (unsigned i = 0; i < 10; ++i) {
            if (remaining_ == 0)
                return false;
            const uint8_t byte = *current_++;
            --remaining_;
            if (i == 9 && byte > 1)
                return false;
            value |= static_cast<uint64_t>(byte & 0x7f) << (7 * i);
            if ((byte & 0x80) == 0)
                return true;
        }
        return false;
    }

    const uint8_t* current_;
    size_t remaining_;
};

bool read_float(const Field& field, float& value)
{
    if (field.wire != 5)
        return false;
    const uint32_t bits = static_cast<uint32_t>(field.bytes[0])
        | static_cast<uint32_t>(field.bytes[1]) << 8
        | static_cast<uint32_t>(field.bytes[2]) << 16
        | static_cast<uint32_t>(field.bytes[3]) << 24;
    static_assert(sizeof(float) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return std::isfinite(value);
}

bool point(const Field& parent, float* output)
{
    if (parent.wire != 2)
        return false;
    output[0] = output[1] = 0;
    Reader reader(parent.bytes, parent.size);
    while (!reader.done()) {
        Field field;
        if (!reader.next(field))
            return false;
        if ((field.number == 1 || field.number == 2)
            && !read_float(field, output[field.number - 1]))
            return false;
    }
    return true;
}

bool quadrilateral(const Field& parent, float* output, unsigned& present)
{
    if (parent.wire != 2)
        return false;
    Reader reader(parent.bytes, parent.size);
    while (!reader.done()) {
        Field field;
        if (!reader.next(field))
            return false;
        if (field.number >= 1 && field.number <= 4) {
            if (!point(field, output + (field.number - 1) * 2))
                return false;
            present |= 1u << (field.number - 1);
        }
    }
    return true;
}

bool line(const Field& parent, RuntimeLine& output)
{
    if (parent.wire != 2)
        return false;
    float rectangle[4] {};
    unsigned corners = 0;
    Reader reader(parent.bytes, parent.size);
    while (!reader.done()) {
        Field field;
        if (!reader.next(field))
            return false;
        if (field.number == 1) {
            if (!quadrilateral(field, output.quad, corners))
                return false;
        } else if (field.number == 2) {
            if (field.wire != 2 || field.size > 4 * 1024 * 1024)
                return false;
            output.text.assign(reinterpret_cast<const char*>(field.bytes), field.size);
        } else if (field.number == 3) {
            if (!read_float(field, output.score))
                return false;
            output.has_score = true;
        } else if (field.number >= 5 && field.number <= 8) {
            if (!read_float(field, rectangle[field.number - 5]))
                return false;
        }
    }
    if (corners != 0x0f) {
        const float l = rectangle[0], t = rectangle[1];
        const float r = rectangle[2], b = rectangle[3];
        const float fallback[] { l, t, r, t, r, b, l, b };
        std::memcpy(output.quad, fallback, sizeof(fallback));
    }
    return true;
}

bool ocr_result(const Field& parent, uint32_t version, RuntimeResult& output)
{
    if (parent.wire != 2)
        return false;
    const uint32_t lines_field = version == 3 ? 1 : 3;
    const uint32_t width_field = version == 3 ? 2 : 4;
    const uint32_t height_field = version == 3 ? 3 : 5;
    Reader reader(parent.bytes, parent.size);
    while (!reader.done()) {
        Field field;
        if (!reader.next(field))
            return false;
        if (field.number == lines_field) {
            if (output.lines.size() >= 100000)
                return false;
            RuntimeLine item;
            if (!line(field, item))
                return false;
            output.lines.push_back(std::move(item));
        } else if (field.number == width_field || field.number == height_field) {
            if (field.wire != 0 || field.integer > std::numeric_limits<uint32_t>::max())
                return false;
            (field.number == width_field ? output.width : output.height)
                = static_cast<uint32_t>(field.integer);
        }
    }
    return true;
}

void varint(std::vector<uint8_t>& output, uint64_t value)
{
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        output.push_back(static_cast<uint8_t>(byte | (value ? 0x80 : 0)));
    } while (value);
}

void bytes(std::vector<uint8_t>& output, uint32_t number,
           const void* data, size_t size)
{
    varint(output, (static_cast<uint64_t>(number) << 3) | 2);
    varint(output, size);
    const auto* begin = static_cast<const uint8_t*>(data);
    if (size)
        output.insert(output.end(), begin, begin + size);
}

bool parse(uint32_t version, uint32_t request_id, const void* data, size_t size,
           Message& output)
{
    if ((version == 3 && request_id != 1)
        || (version == 4 && request_id != 10001 && request_id != 10011))
        return true;
    Reader reader(data, size);
    if (version == 4 && request_id == 10001) {
        output.kind = Message::Kind::ready;
        while (!reader.done()) {
            Field field;
            if (!reader.next(field))
                return false;
            if (field.number == 1) {
                if (field.wire != 0)
                    return false;
                output.supported = field.integer != 0;
            }
        }
        return true;
    }

    uint64_t type = 0;
    bool has_result = false;
    bool has_error = false;
    while (!reader.done()) {
        Field field;
        if (!reader.next(field))
            return false;
        if (version == 3 && field.number == 1) {
            if (field.wire != 0)
                return false;
            type = field.integer;
        } else if (field.number == (version == 3 ? 2u : 1u)) {
            if (field.wire != 0)
                return false;
            output.task_id = field.integer;
        } else if (field.number == (version == 3 ? 3u : 2u)) {
            if (field.wire != 0)
                return false;
            output.result.vendor_error = static_cast<int32_t>(static_cast<uint32_t>(field.integer));
            has_error = true;
        } else if (field.number == (version == 3 ? 4u : 3u)) {
            if (!ocr_result(field, version, output.result))
                return false;
            has_result = true;
        }
    }
    if (version == 3 && type == 1) {
        output.kind = Message::Kind::ready;
        output.supported = output.result.vendor_error == 0;
    } else if (version == 4 || type == 0) {
        if (!has_result && (!has_error || output.result.vendor_error == 0))
            return false;
        output.kind = Message::Kind::result;
    }
    return true;
}

} // namespace

std::vector<uint8_t> make_request(uint32_t version, uint64_t task_id,
                                  const std::string& image_path)
{
    if ((version != 3 && version != 4) || task_id < 2 || task_id > 0x7fffffff
        || image_path.empty() || image_path.size() > 4 * 1024 * 1024)
        throw std::invalid_argument("Invalid OCR protocol request");
    std::vector<uint8_t> output;
    if (version == 3) {
        varint(output, 2u << 3);
        varint(output, task_id);
        std::vector<uint8_t> input;
        bytes(input, 1, image_path.data(), image_path.size());
        bytes(output, 3, input.data(), input.size());
    } else {
        varint(output, 1u << 3);
        varint(output, task_id);
        bytes(output, 2, image_path.data(), image_path.size());
        // These are optional fields: explicitly transmit the third false flag
        // rather than losing its presence by omitting a default scalar.
        const uint8_t options[] { 0x08, 0x01, 0x10, 0x01, 0x18, 0x00 };
        bytes(output, 6, options, sizeof(options));
    }
    return output;
}

bool decode(uint32_t version, uint32_t request_id, const void* data, size_t size,
            Message& message, std::string& error)
{
    message = {};
    error.clear();
    if (version != 3 && version != 4) {
        error = "Unsupported OCR protocol version";
        return false;
    }
    if ((size && !data) || size > max_message_bytes) {
        error = "OCR response exceeds the message limit or has a null payload";
        return false;
    }
    Message parsed;
    if (!parse(version, request_id, data, size, parsed)) {
        error = "Malformed OCR protobuf response";
        return false;
    }
    message = std::move(parsed);
    return true;
}

} // namespace wco::protocol
