#pragma once
#include "WeChatOCR/WeChatOCR.h"
#include <string_view>

namespace wco {
WcoStatus set_error(WcoStatus status, std::string_view message) noexcept;
void clear_error() noexcept;
bool valid_image(const WcoImageView* image) noexcept;
bool valid_utf8(std::string_view text) noexcept;
WcoStatus set_runtime_metadata(WcoEngine* engine, std::string_view version, std::string_view source) noexcept;
}
