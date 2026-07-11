#include "transport/upstream_transport.hpp"

#ifdef _WIN32
#include "transport/windows/winhttp_transport.hpp"
#endif

#include <utility>

namespace ccs {

namespace {

#ifndef _WIN32
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
    std::shared_ptr<RuntimeMetrics> metrics) {
#ifdef _WIN32
    return std::make_unique<WinHttpTransport>(
        timeouts, max_response_body_size, std::move(metrics));
#else
    (void)timeouts;
    (void)max_response_body_size;
    (void)metrics;
    return std::make_unique<UnsupportedTransport>();
#endif
}

} // namespace ccs
