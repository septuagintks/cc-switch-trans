#include "config/config.hpp"
#include "core/app_service.hpp"

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

    ccs::AppService service(parse_result.config);
    std::string error;
    if (!service.start(error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }
    return service.wait();
}
