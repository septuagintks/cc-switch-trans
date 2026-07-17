#include "transport/upstream_transport.hpp"

#ifdef _WIN32
#include "transport/windows/winhttp_transport.hpp"
#elif defined(__APPLE__)
#include "transport/macos/curl_transport.hpp"
#endif

#include <utility>

namespace ccs {

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)
class UnsupportedTransport final : public UpstreamTransport {
public:
    const char* proxy_mode() const noexcept override {
        return "unsupported";
    }

    HttpResponse forward(
        const HttpRequest&,
        const UpstreamTarget&,
        const CancellationToken&) const override {
        throw ProxyError(
            502,
            "upstream_transport_unavailable",
            "upstream transport is not implemented on this platform");
    }

    HttpResponse forward_streaming(
        const HttpRequest&,
        const UpstreamTarget&,
        const HeaderCallback&,
        const ChunkCallback&,
        const CancellationToken&) const override {
        throw ProxyError(
            502,
            "upstream_transport_unavailable",
            "upstream transport is not implemented on this platform");
    }
};
#endif

} // namespace

ProxyError::ProxyError(int status_code, std::string type, std::string message)
    : std::runtime_error(std::move(message))
    , status_code_(status_code)
    , type_(std::move(type)) {}

int ProxyError::status_code() const {
    return status_code_;
}

const std::string& ProxyError::type() const {
    return type_;
}

std::unique_ptr<UpstreamTransport> make_upstream_transport(
    TimeoutConfig timeouts,
    std::size_t max_response_body_size,
    std::shared_ptr<RuntimeMetrics> metrics,
    std::size_t handle_pool_size,
    std::shared_ptr<InflightMemoryBudget> inflight_budget) {
#ifdef _WIN32
    (void)handle_pool_size;
    return std::make_unique<WinHttpTransport>(
        timeouts,
        max_response_body_size,
        std::move(metrics),
        std::move(inflight_budget));
#elif defined(__APPLE__)
    return std::make_unique<CurlTransport>(
        timeouts,
        max_response_body_size,
        handle_pool_size,
        std::move(metrics),
        std::move(inflight_budget));
#else
    (void)timeouts;
    (void)max_response_body_size;
    (void)metrics;
    (void)handle_pool_size;
    (void)inflight_budget;
    return std::make_unique<UnsupportedTransport>();
#endif
}

} // namespace ccs
