#pragma once

#include "config/config.hpp"
#include "core/http_types.hpp"

#include <stdexcept>
#include <functional>
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
    using HeaderCallback = std::function<bool(const HttpResponse&)>;
    using ChunkCallback = std::function<bool(const std::string&)>;

    explicit Proxy(AppConfig config);

    HttpResponse forward(const HttpRequest& request, const std::string& upstream_path) const;
    HttpResponse forward_streaming(
        const HttpRequest& request,
        const std::string& upstream_path,
        const HeaderCallback& on_headers,
        const ChunkCallback& on_chunk) const;

private:
    AppConfig config_;
};

} // namespace ccs
