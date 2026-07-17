#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ccs::test {

inline std::filesystem::path canonical_temp_directory() {
#ifdef _WIN32
    return std::filesystem::temp_directory_path();
#else
    std::error_code error;
    const auto path = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path(), error);
    if (error) {
        throw std::runtime_error(
            "failed to canonicalize the system temporary directory: "
            + error.message());
    }
    return path;
#endif
}

} // namespace ccs::test
