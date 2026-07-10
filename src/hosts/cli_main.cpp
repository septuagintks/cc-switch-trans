#include "config/app_paths.hpp"
#include "config/config.hpp"
#include "config/profile_store.hpp"
#include "core/app_service.hpp"

#include <iostream>

namespace {

int profile_command(const ccs::ParseResult& command, ccs::ProfileStore& store) {
    std::string error;
    std::string output;
    bool changed = false;

    switch (command.command) {
    case ccs::CliCommandKind::ProfileList:
        std::cout << store.list_json();
        return 0;
    case ccs::CliCommandKind::ProfileShow:
        if (!store.show_json(command.profile_name, output, error)) {
            break;
        }
        std::cout << output;
        return 0;
    case ccs::CliCommandKind::ProfileCreate:
        changed = store.create(command.profile_name, error);
        break;
    case ccs::CliCommandKind::ProfileRemove:
        changed = store.remove(command.profile_name, error);
        break;
    case ccs::CliCommandKind::ProfileUse:
        changed = store.use(command.profile_name, error);
        break;
    case ccs::CliCommandKind::ProfileSet:
        changed = store.set(command.profile_name, command.profile_key, command.profile_value, error);
        break;
    case ccs::CliCommandKind::ProfileUnset:
        changed = store.unset(command.profile_name, command.profile_key, error);
        break;
    default:
        error = "invalid profile command";
        break;
    }

    if (!changed || !store.save(error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    if (command.command == ccs::CliCommandKind::ProfileRemove) {
        std::cout << store.list_json();
        return 0;
    }
    if (!store.show_json(command.profile_name, output, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    std::cout << output;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const auto parse_result = ccs::parse_args(argc, argv);

    if (parse_result.help_requested) {
        ccs::print_help(std::cout);
        return 0;
    }
    if (parse_result.version_requested) {
        ccs::print_version(std::cout);
        return 0;
    }
    if (!parse_result.ok) {
        std::cerr << "error: " << parse_result.error << "\n\n";
        ccs::print_help(std::cerr);
        return 1;
    }

    ccs::AppPaths paths;
    std::string error;
    if (!ccs::resolve_app_paths(paths, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    ccs::ProfileStore store(paths);
    if (!store.load(error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    if (parse_result.command != ccs::CliCommandKind::Run) {
        return profile_command(parse_result, store);
    }

    if (!ccs::ensure_app_directories(paths, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    ccs::ConfigSnapshot snapshot;
    std::string selected_profile;
    if (!store.resolve_run(parse_result, snapshot, selected_profile, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    ccs::print_config_summary(std::cout, *snapshot);
    std::cout << "  profile: " << (selected_profile.empty() ? "<none>" : selected_profile) << "\n";

    ccs::AppService service(std::move(snapshot));
    if (!service.start(error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    return service.wait();
}
