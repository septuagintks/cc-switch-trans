#pragma once

#include "config.hpp"
#include "http_types.hpp"
#include "logger.hpp"
#include "proxy.hpp"

#include <functional>
#include <string>

namespace ccs {

class Server {
public:
    explicit Server(AppConfig config);

    int run();
    HttpResponse handle_request(const HttpRequest& request) const;
    std::string process_raw_request(const std::string& raw, const std::string& client_ip) const;
    bool process_raw_request_to_sender(
        const std::string& raw,
        const std::string& client_ip,
        const std::function<bool(const std::string&)>& sender) const;
    void log_request_error(int status_code, const std::string& type, const std::string& message) const;

private:
    HttpResponse handle_usage_request(const HttpRequest& request) const;

    AppConfig config_;
    Proxy proxy_;
    mutable Logger logger_;
};

} // namespace ccs
