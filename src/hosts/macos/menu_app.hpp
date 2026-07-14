#pragma once

#include "config/app_paths.hpp"

#include <filesystem>

namespace ccs {

int run_menu_application(AppPaths paths, std::filesystem::path executable_path);

} // namespace ccs
