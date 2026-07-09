#pragma once

#include "config.hpp"
#include "http_types.hpp"
#include "logger.hpp"
#include "proxy.hpp"

namespace ccs {

class Server {
public:
    explicit Server(AppConfig config);

    int run();
    HttpResponse handle_request(const HttpRequest& request) const;
    std::string process_raw_request(const std::string& raw, const std::string& client_ip) const;

private:
    AppConfig config_;
    Proxy proxy_;
    mutable Logger logger_;
};

} // namespace ccs
