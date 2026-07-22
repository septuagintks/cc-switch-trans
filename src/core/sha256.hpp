#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace ccs {

std::string sha256_hex(std::string_view content);
bool sha256_file_hex(
    const std::filesystem::path& path,
    std::string& digest,
    std::string& error);

} // namespace ccs
