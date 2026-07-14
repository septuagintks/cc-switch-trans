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

class ProxyError : public std::runtime_error {
public:
    ProxyError(int status_code, std::string type, std::string message);

    int status_code() const;
    const std::string& type() const;

private:
    int status_code_;
    std::string type_;
};

class UpstreamTransport {
public:
    using HeaderCallback = std::function<bool(const HttpResponse&)>;
    using ChunkCallback = std::function<bool(const std::string&)>;

    virtual ~UpstreamTransport() = default;

    virtual const char* proxy_mode() const noexcept = 0;
    virtual HttpResponse forward(
        const HttpRequest& request,
        const UpstreamTarget& target,
        const CancellationToken& cancellation = {}) const = 0;
    virtual HttpResponse forward_streaming(
        const HttpRequest& request,
        const UpstreamTarget& target,
        const HeaderCallback& on_headers,
        const ChunkCallback& on_chunk,
        const CancellationToken& cancellation = {}) const = 0;
};

std::unique_ptr<UpstreamTransport> make_upstream_transport(
    TimeoutConfig timeouts,
    std::size_t max_response_body_size,
    std::shared_ptr<RuntimeMetrics> metrics = {},
    std::size_t handle_pool_size = 32);

} // namespace ccs
