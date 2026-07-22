#include "config/app_paths.hpp"
#include "config/config_cli.hpp"
#include "config/composite_config_repository.hpp"
#include "app/application_controller.hpp"
#include "app/app_service.hpp"

#include <iostream>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

void print_runtime_summary(
    const ccs::RuntimeSnapshot& snapshot,
    const std::string& selected_profile) {
    std::cout
        << "ccs-trans runtime\n"
        << "  listen: http://" << snapshot.application.listener.host << ":"
        << snapshot.application.listener.port << "\n"
        << "  profiles: " << snapshot.profiles.size() << "\n"
        << "  routes: " << snapshot.routes.size() << "\n"
        << "  profile filter: "
        << (selected_profile.empty() ? "<all enabled>" : selected_profile) << "\n"
        << "  workers: " << snapshot.application.runtime.worker_threads << "\n"
        << "  max connections: " << snapshot.application.runtime.max_connections << "\n"
        << "  log: " << snapshot.log_path.string() << "\n";
}

bool is_storage_command(ccs::ConfigCliCommandKind kind) {
    return kind == ccs::ConfigCliCommandKind::StorageStatus
        || kind == ccs::ConfigCliCommandKind::StorageMigrate
        || kind == ccs::ConfigCliCommandKind::StorageVerify;
}

bool stdin_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

bool execute_storage_command(
    const ccs::ConfigCliCommand& command,
    ccs::CompositeConfigRepository& repository,
    std::string& output,
    std::string& error) {
    if (command.kind == ccs::ConfigCliCommandKind::StorageStatus) {
        ccs::StorageStatus status;
        if (!repository.inspect_storage(status, error)) {
            return false;
        }
        output = "state: " + std::string(ccs::storage_state_name(status.state)) + "\n";
        if (status.state == ccs::StorageState::Ready) {
            output += "profile revision: " + std::to_string(status.profile_revision) + "\n";
            output += "migrated from: "
                + status.migrated_from_sha256.value_or("<none>") + "\n";
        }
        if (!status.detail.empty()) {
            output += "detail: " + status.detail + "\n";
        }
        return true;
    }
    if (command.kind == ccs::ConfigCliCommandKind::StorageMigrate) {
        if (!ccs::confirm_storage_replacement(
                command, stdin_is_terminal(), std::cin, std::cerr, error)) {
            return false;
        }
        ccs::MigrationResult result;
        if (!repository.migrate_v2(
                {command.storage_replace}, result, error)) {
            return false;
        }
        output = result.outcome == ccs::MigrationOutcome::Migrated
            ? "storage migrated\n"
            : "storage already migrated\n";
        if (result.replaced_database_backup) {
            output += "replaced database backup: "
                + result.replaced_database_backup->string() + "\n";
        }
        return true;
    }
    if (!repository.verify_storage(error)) {
        return false;
    }
    output = "storage verified\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const auto parsed = ccs::parse_config_cli(argc, argv);
    if (!parsed.ok) {
        std::cerr << "error: " << parsed.error << "\n\n";
        ccs::print_config_cli_help(std::cerr);
        return 1;
    }
    if (parsed.command.kind == ccs::ConfigCliCommandKind::Help) {
        ccs::print_config_cli_help(std::cout);
        return 0;
    }
    if (parsed.command.kind == ccs::ConfigCliCommandKind::Version) {
        ccs::print_config_cli_version(std::cout);
        return 0;
    }

    ccs::AppPaths paths;
    std::string error;
    if (!ccs::resolve_app_paths(paths, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    if (parsed.command.kind != ccs::ConfigCliCommandKind::Run) {
        ccs::CompositeConfigRepository repository(paths);
        std::string output;
        const bool succeeded = is_storage_command(parsed.command.kind)
            ? execute_storage_command(parsed.command, repository, output, error)
            : repository.load(error)
                && ccs::execute_config_cli(parsed.command, repository, output, error);
        if (!succeeded) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        std::cout << output;
        return 0;
    }

    ccs::RuntimeLoadOptions options;
    options.selected_profile = parsed.command.run_profile;
    options.log_level = parsed.command.run_log_level;
    options.log_path = parsed.command.run_log_path;
    ccs::RuntimeSnapshotPtr snapshot;
    if (!ccs::load_runtime_snapshot(paths, options, snapshot, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    print_runtime_summary(*snapshot, parsed.command.run_profile);
    ccs::AppService service(std::move(snapshot));
    if (!service.start(error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    return service.wait();
}
