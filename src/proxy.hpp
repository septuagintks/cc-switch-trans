#pragma once

#include "config.hpp"
#include "http_types.hpp"

#include <stdexcept>
#include <string>

namespace ccs {

class ProxyError : public std::runtime_error {
public:
    ProxyError(int status_code, std::string type, std::string message);

    int status_code() const;
    const std::string& type() const;

private:
    int status_code_;
    std::string type_;
};

class Proxy {
public:
    explicit Proxy(AppConfig config);

    HttpResponse forward(const HttpRequest& request, const std::string& upstream_path) const;

private:
    AppConfig config_;
};

} // namespace ccs
