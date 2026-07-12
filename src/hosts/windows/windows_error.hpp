#pragma once

#include <string>

namespace ccs {

std::string windows_error_message(const std::string& operation, unsigned long code);

} // namespace ccs
