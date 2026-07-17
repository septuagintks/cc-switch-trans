#pragma once

#include "transport/upstream_transport.hpp"

#include <memory>

namespace ccs {

class WinHttpTransport final : public UpstreamTransport {
public:
    WinHttpTransport(
        TimeoutConfig timeouts,
        std::size_t max_response_body_size,
        std::shared_ptr<RuntimeMetrics> metrics = {},
        std::shared_ptr<InflightMemoryBudget> inflight_budget = {});
    ~WinHttpTransport() override;

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

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
