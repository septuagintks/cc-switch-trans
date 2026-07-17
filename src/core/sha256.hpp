#pragma once

#include <string>
#include <string_view>

namespace ccs {

std::string sha256_hex(std::string_view content);

} // namespace ccs
