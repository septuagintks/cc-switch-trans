#pragma once

#include "transport/upstream_transport.hpp"

#include <memory>

namespace ccs {

class CurlTransport final : public UpstreamTransport {
public:
    CurlTransport(
        TimeoutConfig timeouts,
        std::size_t max_response_body_size,
        std::size_t handle_pool_size,
        std::shared_ptr<RuntimeMetrics> metrics = {},
        std::shared_ptr<InflightMemoryBudget> inflight_budget = {});
    ~CurlTransport() override;

    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;

    const char* proxy_mode() const noexcept override;
    HttpResponse forward(
        const HttpRequest& request,
        const UpstreamTarget& target,
        const CancellationToken& cancellation = {}) const override;
    HttpResponse forward_streaming(
        const HttpRequest& request,
        const UpstreamTarget& target,
        const HeaderCallback& on_headers,
        const ChunkCallback& on_chunk,
        const CancellationToken& cancellation = {}) const override;

private:
    struct Impl;

    TimeoutConfig timeouts_;
    std::size_t max_response_body_size_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    std::shared_ptr<InflightMemoryBudget> inflight_budget_;
    std::unique_ptr<Impl> impl_;
};

} // namespace ccs
