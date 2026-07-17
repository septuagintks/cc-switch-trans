#include "hosts/host_platform.hpp"

#include "config/composite_config_repository.hpp"

#include <system_error>

namespace ccs {

bool ensure_config_file(const AppPaths& paths, std::string& error) {
    error.clear();
    if (!ensure_app_directories(paths, error)) {
        return false;
    }

    CompositeConfigRepository repository(paths);
    return repository.load(error);
}

} // namespace ccs
