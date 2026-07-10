#include "config/config.hpp"
#include "server/server.hpp"

#include <iostream>

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

    ccs::print_config_summary(std::cout, parse_result.config);

    ccs::Server server(parse_result.config);
    return server.run();
}
