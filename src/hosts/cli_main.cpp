#include "config/app_paths.hpp"
#include "config/config_cli.hpp"
#include "config/config_store.hpp"
#include "config/runtime_compiler.hpp"
#include "app/app_service.hpp"

#include <iostream>
#include <utility>

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
    ccs::ConfigStore store(paths);
    if (!store.load(error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    if (parsed.command.kind != ccs::ConfigCliCommandKind::Run) {
        std::string output;
        if (!ccs::execute_config_cli(parsed.command, store, output, error)) {
            std::cerr << "error: " << error << "\n";
            return 1;
        }
        std::cout << output;
        return 0;
    }

    if (!ccs::ensure_app_directories(paths, error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    auto document = store.document();
    if (!parsed.command.run_log_level.empty()) {
        document.application.logging.level = parsed.command.run_log_level;
    }
    if (!parsed.command.run_log_path.empty()) {
        document.application.logging.path = parsed.command.run_log_path;
    }
    ccs::RuntimeCompileOptions options;
    if (!parsed.command.run_profile.empty()) {
        options.selected_profile = parsed.command.run_profile;
    }
    ccs::RuntimeSnapshotPtr snapshot;
    ccs::RuntimeCompiler compiler(paths.root);
    if (!compiler.compile(document, options, snapshot, error)) {
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
