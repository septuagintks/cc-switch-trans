#include "gui_ipc/frame_codec.hpp"

#include <algorithm>
#include <limits>

namespace ccs::gui_ipc {

namespace {

std::uint32_t read_little_endian_length(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool continuation(std::uint8_t byte) noexcept {
    return (byte & 0xc0U) == 0x80U;
}

} // namespace

const char* frame_error_name(FrameError error) noexcept {
    switch (error) {
    case FrameError::None: return "none";
    case FrameError::EmptyPayload: return "empty_payload";
    case FrameError::TooLarge: return "too_large";
    case FrameError::InvalidUtf8: return "invalid_utf8";
    case FrameError::Incomplete: return "incomplete";
    case FrameError::DecoderFailed: return "decoder_failed";
    }
    return "unknown";
}

bool valid_utf8(std::string_view content) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(content.data());
    std::size_t index = 0;
    while (index < content.size()) {
        const auto first = bytes[index];
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= content.size() || !continuation(bytes[index + 1])) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= content.size()
                || !continuation(bytes[index + 1])
                || !continuation(bytes[index + 2])) {
                return false;
            }
            const auto second = bytes[index + 1];
            if ((first == 0xe0U && second < 0xa0U)
                || (first == 0xedU && second >= 0xa0U)) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= content.size()
                || !continuation(bytes[index + 1])
                || !continuation(bytes[index + 2])
                || !continuation(bytes[index + 3])) {
                return false;
            }
            const auto second = bytes[index + 1];
            if ((first == 0xf0U && second < 0x90U)
                || (first == 0xf4U && second >= 0x90U)) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

bool encode_frame(
    std::string_view payload,
    std::vector<std::uint8_t>& frame,
    FrameError& error) {
    frame.clear();
    error = FrameError::None;
    if (payload.empty()) {
        error = FrameError::EmptyPayload;
        return false;
    }
    if (payload.size() > kMaximumFrameBytes
        || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = FrameError::TooLarge;
        return false;
    }
    if (!valid_utf8(payload)) {
        error = FrameError::InvalidUtf8;
        return false;
    }
    const auto length = static_cast<std::uint32_t>(payload.size());
    frame.reserve(4 + payload.size());
    frame.push_back(static_cast<std::uint8_t>(length & 0xffU));
    frame.push_back(static_cast<std::uint8_t>((length >> 8U) & 0xffU));
    frame.push_back(static_cast<std::uint8_t>((length >> 16U) & 0xffU));
    frame.push_back(static_cast<std::uint8_t>((length >> 24U) & 0xffU));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return true;
}

bool FrameDecoder::consume(
    std::span<const std::uint8_t> bytes,
    std::vector<std::string>& frames,
    FrameError& error) {
    error = FrameError::None;
    if (failed_) {
        error = FrameError::DecoderFailed;
        return false;
    }
    if (bytes.empty()) {
        return true;
    }
    while (!bytes.empty()) {
        if (offset_ != 0) {
            buffer_.erase(
                buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
            offset_ = 0;
        }
        const auto maximum_buffer = kMaximumFrameBytes + 4;
        if (buffer_.size() > maximum_buffer) {
            failed_ = true;
            error = FrameError::TooLarge;
            return false;
        }
        const auto capacity = maximum_buffer - buffer_.size();
        const auto count = std::min(capacity, bytes.size());
        buffer_.insert(
            buffer_.end(), bytes.begin(), bytes.begin() + count);
        bytes = bytes.subspan(count);
        if (!decode_available(frames, error)) return false;
        if (count == 0 && !bytes.empty()) {
            failed_ = true;
            error = FrameError::TooLarge;
            return false;
        }
    }
    return true;
}

bool FrameDecoder::decode_available(
    std::vector<std::string>& frames,
    FrameError& error) {
    while (buffer_.size() - offset_ >= 4) {
        const auto length = read_little_endian_length(buffer_.data() + offset_);
        if (length == 0) {
            failed_ = true;
            error = FrameError::EmptyPayload;
            return false;
        }
        if (length > kMaximumFrameBytes) {
            failed_ = true;
            error = FrameError::TooLarge;
            return false;
        }
        const auto available = buffer_.size() - offset_ - 4;
        if (available < length) {
            return true;
        }
        const auto* payload = reinterpret_cast<const char*>(buffer_.data() + offset_ + 4);
        const std::string_view view(payload, length);
        if (!valid_utf8(view)) {
            failed_ = true;
            error = FrameError::InvalidUtf8;
            return false;
        }
        frames.emplace_back(view);
        offset_ += 4 + length;
    }
    if (offset_ == buffer_.size()) {
        buffer_.clear();
        offset_ = 0;
    } else if (offset_ > 64U * 1024U) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
    return true;
}

bool FrameDecoder::finish(FrameError& error) const noexcept {
    if (failed_) {
        error = FrameError::DecoderFailed;
        return false;
    }
    if (buffer_.size() != offset_) {
        error = FrameError::Incomplete;
        return false;
    }
    error = FrameError::None;
    return true;
}

void FrameDecoder::reset() noexcept {
    buffer_.clear();
    offset_ = 0;
    failed_ = false;
}

std::size_t FrameDecoder::buffered_bytes() const noexcept {
    return buffer_.size() - offset_;
}

bool FrameDecoder::failed() const noexcept {
    return failed_;
}

} // namespace ccs::gui_ipc
