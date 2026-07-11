#pragma once

#include "core/cancellation.hpp"
#include "core/http_types.hpp"
#include "core/runtime_metrics.hpp"
#include "core/timeouts.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace ccs {

const char* upstream_proxy_mode();

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

    Proxy(
        TimeoutConfig timeouts,
        std::size_t max_response_body_size,
        std::shared_ptr<RuntimeMetrics> metrics = {});
    ~Proxy();

    Proxy(const Proxy&) = delete;
    Proxy& operator=(const Proxy&) = delete;

    HttpResponse forward(
        const HttpRequest& request,
        const UpstreamTarget& target,
        const CancellationToken& cancellation = {}) const;
    HttpResponse forward_streaming(
        const HttpRequest& request,
        const UpstreamTarget& target,
        const HeaderCallback& on_headers,
        const ChunkCallback& on_chunk,
        const CancellationToken& cancellation = {}) const;

private:
    struct Impl;

    TimeoutConfig timeouts_;
    std::size_t max_response_body_size_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    std::unique_ptr<Impl> impl_;
};

} // namespace ccs
