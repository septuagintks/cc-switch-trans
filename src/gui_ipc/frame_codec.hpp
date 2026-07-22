#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ccs::gui_ipc {

enum class FrameError {
    None,
    EmptyPayload,
    TooLarge,
    InvalidUtf8,
    Incomplete,
    DecoderFailed,
};

const char* frame_error_name(FrameError error) noexcept;
bool valid_utf8(std::string_view content) noexcept;

bool encode_frame(
    std::string_view payload,
    std::vector<std::uint8_t>& frame,
    FrameError& error);

class FrameDecoder {
public:
    bool consume(
        std::span<const std::uint8_t> bytes,
        std::vector<std::string>& frames,
        FrameError& error);
    bool finish(FrameError& error) const noexcept;
    void reset() noexcept;
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

private:
    bool decode_available(std::vector<std::string>& frames, FrameError& error);

    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

} // namespace ccs::gui_ipc
