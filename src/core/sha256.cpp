#include "core/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace ccs {

namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t rotate_right(std::uint32_t value, unsigned int count) {
    return (value >> count) | (value << (32U - count));
}

class Sha256 final {
public:
    bool update(std::string_view content) {
        if (content.size() > std::numeric_limits<std::uint64_t>::max() - total_bytes_) {
            return false;
        }
        total_bytes_ += static_cast<std::uint64_t>(content.size());
        std::size_t offset = 0;
        if (buffer_size_ != 0) {
            const auto copied = std::min(content.size(), buffer_.size() - buffer_size_);
            std::copy_n(content.data(), copied, buffer_.data() + buffer_size_);
            buffer_size_ += copied;
            offset += copied;
            if (buffer_size_ == buffer_.size()) {
                transform(buffer_.data());
                buffer_size_ = 0;
            }
        }
        while (content.size() - offset >= buffer_.size()) {
            transform(reinterpret_cast<const std::uint8_t*>(content.data() + offset));
            offset += buffer_.size();
        }
        if (offset < content.size()) {
            buffer_size_ = content.size() - offset;
            std::copy_n(content.data() + offset, buffer_size_, buffer_.data());
        }
        return true;
    }

    std::string finish() {
        const auto bit_length = total_bytes_ * 8U;
        buffer_[buffer_size_++] = 0x80;
        if (buffer_size_ > 56) {
            std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                buffer_.end(), 0);
            transform(buffer_.data());
            buffer_size_ = 0;
        }
        std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
            buffer_.begin() + 56, 0);
        for (std::size_t index = 0; index < 8; ++index) {
            buffer_[56 + index] = static_cast<std::uint8_t>(
                bit_length >> ((7U - index) * 8U));
        }
        transform(buffer_.data());

        std::ostringstream result;
        result << std::hex << std::setfill('0');
        for (const auto value : hash_) {
            result << std::setw(8) << value;
        }
        return result.str();
    }

private:
    void transform(const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t index = 0; index < 16; ++index) {
            const auto position = index * 4;
            words[index] = (static_cast<std::uint32_t>(block[position]) << 24)
                | (static_cast<std::uint32_t>(block[position + 1]) << 16)
                | (static_cast<std::uint32_t>(block[position + 2]) << 8)
                | static_cast<std::uint32_t>(block[position + 3]);
        }
        for (std::size_t index = 16; index < words.size(); ++index) {
            const auto sigma0 = rotate_right(words[index - 15], 7)
                ^ rotate_right(words[index - 15], 18)
                ^ (words[index - 15] >> 3);
            const auto sigma1 = rotate_right(words[index - 2], 17)
                ^ rotate_right(words[index - 2], 19)
                ^ (words[index - 2] >> 10);
            words[index] = words[index - 16] + sigma0 + words[index - 7] + sigma1;
        }

        auto a = hash_[0];
        auto b = hash_[1];
        auto c = hash_[2];
        auto d = hash_[3];
        auto e = hash_[4];
        auto f = hash_[5];
        auto g = hash_[6];
        auto h = hash_[7];
        for (std::size_t index = 0; index < words.size(); ++index) {
            const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11)
                ^ rotate_right(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temporary1 = h + sum1 + choice + kRoundConstants[index]
                + words[index];
            const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13)
                ^ rotate_right(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporary2 = sum0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporary1;
            d = c;
            c = b;
            b = a;
            a = temporary1 + temporary2;
        }
        hash_[0] += a;
        hash_[1] += b;
        hash_[2] += c;
        hash_[3] += d;
        hash_[4] += e;
        hash_[5] += f;
        hash_[6] += g;
        hash_[7] += h;
    }

    std::array<std::uint32_t, 8> hash_ = {
        0x6a09e667,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19,
    };
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffer_size_ = 0;
    std::uint64_t total_bytes_ = 0;
};

} // namespace

std::string sha256_hex(std::string_view content) {
    Sha256 state;
    (void)state.update(content);
    return state.finish();
}

bool sha256_file_hex(
    const std::filesystem::path& path,
    std::string& digest,
    std::string& error) {
    digest.clear();
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "failed to open file for SHA-256: " + path.string();
        return false;
    }
    Sha256 state;
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0
            && !state.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)))) {
            error = "file is too large for SHA-256 length accounting: " + path.string();
            return false;
        }
    }
    if (!input.eof()) {
        error = "failed to read file for SHA-256: " + path.string();
        return false;
    }
    digest = state.finish();
    return true;
}

} // namespace ccs
