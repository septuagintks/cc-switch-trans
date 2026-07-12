#include "hosts/host_platform.hpp"

#include "config/config_store.hpp"

#include <system_error>

namespace ccs {

bool ensure_config_file(const AppPaths& paths, std::string& error) {
    error.clear();
    if (!ensure_app_directories(paths, error)) {
        return false;
    }

    ConfigStore store(paths);
    if (!store.load(error)) {
        return false;
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(paths.config_file, ec);
    if (ec) {
        error = "failed to inspect config file: " + ec.message();
        return false;
    }
    if (exists) {
        return true;
    }
    return store.save(store.document(), error);
}

} // namespace ccs
