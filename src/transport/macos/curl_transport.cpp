#include "transport/macos/curl_transport.hpp"

#include "core/url.hpp"
#include "transport/header_filter.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <utility>
#include <vector>

#ifndef __APPLE__
#error "CurlTransport is a macOS-only source file"
#endif

namespace ccs {

namespace {

using Clock = std::chrono::steady_clock;
constexpr int kPollIntervalMs = 25;
constexpr long kConnectionsPerSlot = 4;
constexpr std::size_t kResponseHeaderLimit = 64 * 1024;

class CurlGlobal {
public:
    CurlGlobal() {
        const auto result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error(
                "failed to initialize system libcurl: "
                + std::string(curl_easy_strerror(result)));
        }
    }

    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

void ensure_curl_global() {
    static CurlGlobal global;
    (void)global;
}

std::string trim_header_value(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
                    return !is_space(static_cast<unsigned char>(ch));
                }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
                    return !is_space(static_cast<unsigned char>(ch));
                }).base(),
        value.end());
    return value;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_event_stream(const Headers& headers) {
    for (const auto& [name, value] : headers) {
        if (lower_copy(name) == "content-type"
            && lower_copy(value).find("text/event-stream") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string build_url(const ParsedUrl& upstream, const std::string& path) {
    std::ostringstream out;
    out << (upstream.secure ? "https://" : "http://");
    if (upstream.host.find(':') != std::string::npos) {
        out << "[" << upstream.host << "]";
    } else {
        out << upstream.host;
    }
    out << ":" << upstream.port << path;
    return out.str();
}

class CurlHeaders {
public:
    CurlHeaders() = default;

    ~CurlHeaders() {
        curl_slist_free_all(headers_);
    }

    CurlHeaders(const CurlHeaders&) = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;
    CurlHeaders(CurlHeaders&&) = delete;
    CurlHeaders& operator=(CurlHeaders&&) = delete;

    void append(const std::string& value) {
        curl_slist* next = curl_slist_append(headers_, value.c_str());
        if (next == nullptr) {
            throw ProxyError(500, "server_error", "failed to allocate upstream headers");
        }
        headers_ = next;
    }

    curl_slist* get() const noexcept {
        return headers_;
    }

private:
    curl_slist* headers_ = nullptr;
};

struct CurlSlot {
    CURL* easy = nullptr;
    CURLM* multi = nullptr;
    bool attached = false;

    ~CurlSlot() {
        if (attached && multi != nullptr && easy != nullptr) {
            if (curl_multi_remove_handle(multi, easy) != CURLM_OK) {
                // libcurl forbids cleaning an easy handle that is still attached.
                // The pool is poisoned before this path, so abandoning both handles
                // is bounded and safer than invoking cleanup with invalid ownership.
                easy = nullptr;
                multi = nullptr;
                return;
            }
            attached = false;
        }
        if (easy != nullptr) {
            curl_easy_cleanup(easy);
        }
        if (multi != nullptr) {
            curl_multi_cleanup(multi);
        }
    }
};

struct CurlEasyDeleter {
    void operator()(CURL* easy) const noexcept {
        if (easy != nullptr) {
            curl_easy_cleanup(easy);
        }
    }
};

struct CurlMultiDeleter {
    void operator()(CURLM* multi) const noexcept {
        if (multi != nullptr) {
            curl_multi_cleanup(multi);
        }
    }
};

class CurlHandlePool {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(CurlHandlePool* pool, CurlSlot* slot)
            : pool_(pool)
            , slot_(slot) {}

        ~Lease() {
            reset();
        }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : pool_(std::exchange(other.pool_, nullptr))
            , slot_(std::exchange(other.slot_, nullptr)) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                reset();
                pool_ = std::exchange(other.pool_, nullptr);
                slot_ = std::exchange(other.slot_, nullptr);
            }
            return *this;
        }

        CurlSlot& get() const {
            return *slot_;
        }

        void discard() noexcept {
            if (pool_ != nullptr) {
                pool_->discard(slot_);
                pool_ = nullptr;
                slot_ = nullptr;
            }
        }

    private:
        void reset() noexcept {
            if (pool_ != nullptr) {
                pool_->release(slot_);
                pool_ = nullptr;
                slot_ = nullptr;
            }
        }

        CurlHandlePool* pool_ = nullptr;
        CurlSlot* slot_ = nullptr;
    };

    CurlHandlePool(std::size_t limit, std::shared_ptr<RuntimeMetrics> metrics)
        : limit_(std::max<std::size_t>(1, limit))
        , metrics_(std::move(metrics)) {
        slots_.reserve(limit_);
        available_.reserve(limit_);
    }

    Lease acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
            return poisoned_ || !available_.empty() || created_ < limit_;
        });
        if (poisoned_) {
            throw ProxyError(
                500, "server_error", "system libcurl handle pool is unavailable");
        }
        if (!available_.empty()) {
            auto* slot = available_.back();
            available_.pop_back();
            curl_easy_reset(slot->easy);
            return Lease(this, slot);
        }

        std::unique_ptr<CURL, CurlEasyDeleter> easy(curl_easy_init());
        if (!easy) {
            throw ProxyError(500, "server_error", "failed to create a system libcurl easy handle");
        }
        std::unique_ptr<CURLM, CurlMultiDeleter> multi(curl_multi_init());
        if (!multi) {
            throw ProxyError(500, "server_error", "failed to create a system libcurl multi handle");
        }
        if (curl_multi_setopt(multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, kConnectionsPerSlot)
                != CURLM_OK
            || curl_multi_setopt(multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS, kConnectionsPerSlot)
                != CURLM_OK) {
            throw ProxyError(500, "server_error", "failed to bound the libcurl connection cache");
        }
        auto owned_slot = std::make_unique<CurlSlot>();
        owned_slot->easy = easy.release();
        owned_slot->multi = multi.release();
        auto* slot = owned_slot.get();
        slots_.push_back(std::move(owned_slot));
        ++created_;
        if (metrics_) {
            metrics_->upstream_request_handle_created();
        }
        return Lease(this, slot);
    }

private:
    void release(CurlSlot* slot) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        available_.push_back(slot);
        cv_.notify_one();
    }

    void discard(CurlSlot* slot) noexcept {
        std::unique_ptr<CurlSlot> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            poisoned_ = true;
            const auto found = std::find_if(
                slots_.begin(), slots_.end(), [slot](const auto& candidate) {
                    return candidate.get() == slot;
                });
            if (found != slots_.end()) {
                discarded = std::move(*found);
                slots_.erase(found);
                --created_;
            }
        }
        cv_.notify_all();
    }

    std::size_t limit_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<CurlSlot>> slots_;
    std::vector<CurlSlot*> available_;
    std::size_t created_ = 0;
    bool poisoned_ = false;
};

enum class AbortReason {
    None,
    Cancelled,
    ResolveTimeout,
    ConnectTimeout,
    SendTimeout,
    ResponseHeaderTimeout,
    StreamIdleTimeout,
    ResponseBodyTimeout,
    TotalTimeout,
    ResponseTooLarge,
    ResponseHeadersTooLarge,
    InflightBudgetExceeded,
    AllocationFailed,
    CallbackException,
    CallbackStopped,
};

struct RequestContext {
    CURL* easy = nullptr;
    const TimeoutConfig* timeouts = nullptr;
    std::size_t max_response_body_size = 0;
    std::size_t request_body_size = 0;
    std::shared_ptr<RuntimeMetrics> metrics;
    std::shared_ptr<InflightMemoryBudget> inflight_budget;
    std::shared_ptr<InflightMemoryBudget::Lease> response_body_memory;
    std::size_t response_header_bytes = 0;
    std::exception_ptr callback_exception;
    bool callback_exception_after_headers = false;
    const CancellationToken* cancellation = nullptr;
    const UpstreamTransport::HeaderCallback* on_headers = nullptr;
    const UpstreamTransport::ChunkCallback* on_chunk = nullptr;
    HttpResponse response;
    Headers response_headers;
    Clock::time_point started = Clock::now();
    Clock::time_point resolved_at{};
    Clock::time_point connected_at{};
    Clock::time_point send_completed_at{};
    Clock::time_point last_body_activity{};
    curl_off_t uploaded = 0;
    AbortReason abort_reason = AbortReason::None;
    bool resolved = false;
    bool connected = false;
    bool send_completed = false;
    bool headers_complete = false;
    bool streaming = false;
};

template <typename Value>
void set_easy_option(CURL* easy, CURLoption option, Value value) {
    const auto result = curl_easy_setopt(easy, option, value);
    if (result != CURLE_OK) {
        throw ProxyError(
            500,
            "server_error",
            "failed to configure system libcurl: " + std::string(curl_easy_strerror(result)));
    }
}

void mark_resolved(RequestContext& context, Clock::time_point now) {
    if (!context.resolved) {
        context.resolved = true;
        context.resolved_at = now;
    }
}

void mark_connected(RequestContext& context, Clock::time_point now) {
    mark_resolved(context, now);
    if (!context.connected) {
        context.connected = true;
        context.connected_at = now;
    }
}

void mark_send_completed(RequestContext& context, Clock::time_point now) {
    mark_connected(context, now);
    if (!context.send_completed) {
        context.send_completed = true;
        context.send_completed_at = now;
    }
}

void reset_response_accounting(RequestContext& context) noexcept {
    if (context.response_body_memory && context.response_header_bytes != 0) {
        context.response_body_memory->shrink(context.response_header_bytes);
    }
    context.response_header_bytes = 0;
    if (context.response_body_memory && !context.response.body.empty()) {
        context.response_body_memory->shrink(context.response.body.size());
    }
}

bool reserve_response_header_bytes(
    RequestContext& context,
    std::size_t bytes) noexcept {
    if (context.response_header_bytes > kResponseHeaderLimit
        || bytes > kResponseHeaderLimit - context.response_header_bytes) {
        context.abort_reason = AbortReason::ResponseHeadersTooLarge;
        return false;
    }
    if (context.response_body_memory
        && !context.response_body_memory->try_grow(bytes)) {
        context.abort_reason = AbortReason::InflightBudgetExceeded;
        return false;
    }
    context.response_header_bytes += bytes;
    return true;
}

void record_callback_exception(
    RequestContext& context,
    bool after_headers) noexcept {
    context.callback_exception = std::current_exception();
    context.callback_exception_after_headers = after_headers;
    context.abort_reason = AbortReason::CallbackException;
}

int socket_option_callback(void* client, curl_socket_t socket, curlsocktype) {
    auto& context = *static_cast<RequestContext*>(client);
    mark_resolved(context, Clock::now());
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0) {
        return CURL_SOCKOPT_ERROR;
    }
#endif
    if (context.metrics) {
        context.metrics->upstream_connection_handle_created();
    }
    return CURL_SOCKOPT_OK;
}

int transfer_progress(
    void* client,
    curl_off_t,
    curl_off_t,
    curl_off_t upload_total,
    curl_off_t uploaded) {
    auto& context = *static_cast<RequestContext*>(client);
    if (context.cancellation->is_cancelled()) {
        context.abort_reason = AbortReason::Cancelled;
        return 1;
    }
    if (uploaded > context.uploaded && context.metrics) {
        context.metrics->upstream_bytes_sent(
            static_cast<std::size_t>(uploaded - context.uploaded));
    }
    context.uploaded = uploaded;

    const auto now = Clock::now();
    curl_off_t lookup_time = 0;
    if (!context.resolved
        && curl_easy_getinfo(context.easy, CURLINFO_NAMELOOKUP_TIME_T, &lookup_time) == CURLE_OK
        && lookup_time > 0) {
        mark_resolved(context, context.started + std::chrono::microseconds(lookup_time));
    }
    curl_off_t connect_time = 0;
    curl_off_t pretransfer_time = 0;
    if (!context.connected
        && ((curl_easy_getinfo(context.easy, CURLINFO_CONNECT_TIME_T, &connect_time) == CURLE_OK
                && connect_time > 0)
            || (curl_easy_getinfo(context.easy, CURLINFO_PRETRANSFER_TIME_T, &pretransfer_time)
                    == CURLE_OK
                && pretransfer_time > 0))) {
        const auto elapsed = std::max(connect_time, pretransfer_time);
        mark_connected(context, context.started + std::chrono::microseconds(elapsed));
    }
    if (context.connected && !context.send_completed
        && (context.request_body_size == 0
            || (upload_total > 0
                && uploaded >= static_cast<curl_off_t>(context.request_body_size)))) {
        mark_send_completed(context, now);
    }
    return 0;
}

std::size_t response_header_callback(
    char* data,
    std::size_t size,
    std::size_t count,
    void* client) noexcept {
    auto& context = *static_cast<RequestContext*>(client);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        context.abort_reason = AbortReason::ResponseHeadersTooLarge;
        return 0;
    }
    const std::size_t bytes = size * count;
    try {
        const std::string_view raw(data, bytes);
        const auto now = Clock::now();

        if (raw.starts_with("HTTP/")) {
            reset_response_accounting(context);
            Headers{}.swap(context.response_headers);
            if (!reserve_response_header_bytes(context, bytes)) {
                return 0;
            }
            context.response = HttpResponse{};
            context.response.body_memory = context.response_body_memory;
            context.response.reason.clear();
            context.headers_complete = false;
            context.streaming = false;
            std::istringstream line{std::string(raw)};
            std::string version;
            line >> version >> context.response.status_code;
            std::string reason;
            std::getline(line, reason);
            context.response.reason = trim_header_value(std::move(reason));
            mark_send_completed(context, now);
            return bytes;
        }

        if (!reserve_response_header_bytes(context, bytes)) {
            return 0;
        }
        if (context.headers_complete) {
            return bytes;
        }

        if (raw == "\r\n" || raw == "\n") {
            if (context.response.status_code >= 100
                && context.response.status_code < 200) {
                return bytes;
            }
            std::optional<InflightMemoryBudget::Lease> filter_memory;
            if (context.inflight_budget && context.response_header_bytes != 0) {
                filter_memory = context.inflight_budget->try_acquire(
                    context.response_header_bytes);
                if (!filter_memory) {
                    context.abort_reason = AbortReason::InflightBudgetExceeded;
                    return 0;
                }
            }
            context.response.headers = filter_response_headers(context.response_headers);
            Headers{}.swap(context.response_headers);
            if (filter_memory) {
                filter_memory->reset();
            }
            context.headers_complete = true;
            context.streaming = is_event_stream(context.response.headers)
                && context.on_chunk != nullptr && static_cast<bool>(*context.on_chunk);
            context.last_body_activity = now;
            if (context.streaming && context.on_headers != nullptr
                && static_cast<bool>(*context.on_headers)) {
                try {
                    if (!(*context.on_headers)(context.response)) {
                        context.abort_reason = AbortReason::CallbackStopped;
                        return 0;
                    }
                } catch (...) {
                    record_callback_exception(context, false);
                    return 0;
                }
            }
            return bytes;
        }

        const auto colon = raw.find(':');
        if (colon != std::string_view::npos && colon != 0) {
            context.response_headers.emplace_back(
                trim_header_value(std::string(raw.substr(0, colon))),
                trim_header_value(std::string(raw.substr(colon + 1))));
        }
        return bytes;
    } catch (const InflightBudgetExceeded&) {
        record_callback_exception(context, false);
    } catch (const std::bad_alloc&) {
        context.abort_reason = AbortReason::AllocationFailed;
    } catch (...) {
        record_callback_exception(context, false);
    }
    return 0;
}

std::size_t response_body_callback(
    char* data,
    std::size_t size,
    std::size_t count,
    void* client) noexcept {
    auto& context = *static_cast<RequestContext*>(client);
    if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
        context.abort_reason = AbortReason::ResponseTooLarge;
        return 0;
    }
    const std::size_t bytes = size * count;
    try {
        if (context.cancellation->is_cancelled()) {
            context.abort_reason = AbortReason::Cancelled;
            return 0;
        }
        context.last_body_activity = Clock::now();
        if (context.metrics) {
            context.metrics->upstream_bytes_received(bytes);
        }
        if (context.streaming) {
            std::optional<InflightMemoryBudget::Lease> memory;
            if (context.inflight_budget) {
                memory = context.inflight_budget->try_acquire(bytes);
                if (!memory) {
                    context.abort_reason = AbortReason::InflightBudgetExceeded;
                    return 0;
                }
            }
            std::string chunk(data, bytes);
            try {
                if (!(*context.on_chunk)(chunk)) {
                    context.abort_reason = AbortReason::CallbackStopped;
                    return 0;
                }
            } catch (...) {
                record_callback_exception(context, true);
                return 0;
            }
            return bytes;
        }
        const auto remaining = context.max_response_body_size
            - std::min(context.max_response_body_size, context.response.body.size());
        if (bytes > remaining) {
            context.abort_reason = AbortReason::ResponseTooLarge;
            return 0;
        }
        if (context.response_body_memory
            && !context.response_body_memory->try_grow(bytes)) {
            context.abort_reason = AbortReason::InflightBudgetExceeded;
            return 0;
        }
        try {
            context.response.body.append(data, bytes);
        } catch (...) {
            if (context.response_body_memory) {
                context.response_body_memory->shrink(bytes);
            }
            throw;
        }
        return bytes;
    } catch (const InflightBudgetExceeded&) {
        record_callback_exception(context, context.streaming);
    } catch (const std::bad_alloc&) {
        context.abort_reason = AbortReason::AllocationFailed;
    } catch (...) {
        record_callback_exception(context, context.streaming);
    }
    return 0;
}

bool elapsed_at_least(
    Clock::time_point now,
    Clock::time_point start,
    int timeout_ms) {
    return now - start >= std::chrono::milliseconds(timeout_ms);
}

AbortReason deadline_abort(RequestContext& context) {
    if (context.cancellation->is_cancelled()) {
        return AbortReason::Cancelled;
    }
    const auto now = Clock::now();
    if (context.timeouts->total_ms > 0
        && elapsed_at_least(now, context.started, context.timeouts->total_ms)) {
        return AbortReason::TotalTimeout;
    }
    if (!context.resolved) {
        return elapsed_at_least(now, context.started, context.timeouts->resolve_ms)
            ? AbortReason::ResolveTimeout
            : AbortReason::None;
    }
    if (!context.connected) {
        return elapsed_at_least(now, context.resolved_at, context.timeouts->connect_ms)
            ? AbortReason::ConnectTimeout
            : AbortReason::None;
    }
    if (!context.send_completed) {
        return elapsed_at_least(now, context.connected_at, context.timeouts->send_ms)
            ? AbortReason::SendTimeout
            : AbortReason::None;
    }
    if (!context.headers_complete) {
        return elapsed_at_least(
                   now, context.send_completed_at, context.timeouts->response_header_ms)
            ? AbortReason::ResponseHeaderTimeout
            : AbortReason::None;
    }
    const int body_timeout = context.streaming
        ? context.timeouts->stream_idle_ms
        : context.timeouts->response_header_ms;
    if (elapsed_at_least(now, context.last_body_activity, body_timeout)) {
        return context.streaming
            ? AbortReason::StreamIdleTimeout
            : AbortReason::ResponseBodyTimeout;
    }
    return AbortReason::None;
}

ProxyError abort_error(AbortReason reason) {
    switch (reason) {
    case AbortReason::Cancelled:
        return ProxyError(499, "client_cancelled", "client disconnected");
    case AbortReason::ResolveTimeout:
        return ProxyError(504, "upstream_resolve_timeout", "upstream name resolution timed out");
    case AbortReason::ConnectTimeout:
        return ProxyError(504, "upstream_connect_timeout", "upstream connection timed out");
    case AbortReason::SendTimeout:
        return ProxyError(504, "upstream_send_timeout", "upstream request send timed out");
    case AbortReason::ResponseHeaderTimeout:
        return ProxyError(504, "upstream_response_header_timeout", "upstream response header timed out");
    case AbortReason::StreamIdleTimeout:
        return ProxyError(504, "upstream_stream_idle_timeout", "upstream event stream timed out");
    case AbortReason::ResponseBodyTimeout:
        return ProxyError(504, "upstream_receive_timeout", "upstream response timed out");
    case AbortReason::TotalTimeout:
        return ProxyError(504, "upstream_total_timeout", "upstream request total timeout exceeded");
    case AbortReason::ResponseTooLarge:
        return ProxyError(502, "upstream_response_too_large", "upstream response body too large");
    case AbortReason::ResponseHeadersTooLarge:
        return ProxyError(
            502,
            "upstream_response_headers_too_large",
            "upstream response headers too large");
    case AbortReason::InflightBudgetExceeded:
        return ProxyError(503, "server_error", "inflight memory budget exhausted");
    case AbortReason::AllocationFailed:
        return ProxyError(500, "server_error", "failed to allocate upstream response buffer");
    case AbortReason::None:
    case AbortReason::CallbackException:
    case AbortReason::CallbackStopped:
        break;
    }
    return ProxyError(502, "upstream_error", "upstream request failed");
}

UpstreamTimeoutPhase timeout_phase(AbortReason reason) {
    switch (reason) {
    case AbortReason::ResolveTimeout:
        return UpstreamTimeoutPhase::Resolve;
    case AbortReason::ConnectTimeout:
        return UpstreamTimeoutPhase::Connect;
    case AbortReason::SendTimeout:
        return UpstreamTimeoutPhase::Send;
    case AbortReason::ResponseHeaderTimeout:
        return UpstreamTimeoutPhase::ResponseHeader;
    case AbortReason::StreamIdleTimeout:
        return UpstreamTimeoutPhase::StreamIdle;
    case AbortReason::ResponseBodyTimeout:
        return UpstreamTimeoutPhase::ResponseBody;
    case AbortReason::TotalTimeout:
        return UpstreamTimeoutPhase::Total;
    case AbortReason::None:
    case AbortReason::Cancelled:
    case AbortReason::ResponseTooLarge:
    case AbortReason::ResponseHeadersTooLarge:
    case AbortReason::InflightBudgetExceeded:
    case AbortReason::AllocationFailed:
    case AbortReason::CallbackException:
    case AbortReason::CallbackStopped:
        break;
    }
    return UpstreamTimeoutPhase::Total;
}

bool is_timeout(AbortReason reason) {
    return reason >= AbortReason::ResolveTimeout
        && reason <= AbortReason::TotalTimeout;
}

} // namespace

struct CurlTransport::Impl {
    Impl(std::size_t handle_pool_size, const std::shared_ptr<RuntimeMetrics>& metrics)
        : pool(handle_pool_size, metrics) {}

    CurlHandlePool pool;
};

CurlTransport::CurlTransport(
    TimeoutConfig timeouts,
    std::size_t max_response_body_size,
    std::size_t handle_pool_size,
    std::shared_ptr<RuntimeMetrics> metrics,
    std::shared_ptr<InflightMemoryBudget> inflight_budget)
    : timeouts_(timeouts)
    , max_response_body_size_(max_response_body_size)
    , metrics_(std::move(metrics))
    , inflight_budget_(std::move(inflight_budget)) {
    ensure_curl_global();
    impl_ = std::make_unique<Impl>(handle_pool_size, metrics_);
}

CurlTransport::~CurlTransport() = default;

const char* CurlTransport::proxy_mode() const noexcept {
    return "macos_environment";
}

HttpResponse CurlTransport::forward(
    const HttpRequest& request,
    const UpstreamTarget& target,
    const CancellationToken& cancellation) const {
    return forward_streaming(request, target, {}, {}, cancellation);
}

HttpResponse CurlTransport::forward_streaming(
    const HttpRequest& request,
    const UpstreamTarget& target,
    const HeaderCallback& on_headers,
    const ChunkCallback& on_chunk,
    const CancellationToken& cancellation) const {
    if (metrics_) {
        metrics_->upstream_request_started();
    }
    if (cancellation.is_cancelled()) {
        if (metrics_) {
            metrics_->upstream_request_cancelled();
        }
        throw ProxyError(499, "client_cancelled", "client disconnected");
    }

    ParsedUrl upstream;
    try {
        upstream = parse_http_url(target.base_url);
    } catch (const std::exception& ex) {
        throw ProxyError(500, "configuration_error", ex.what());
    }
    const auto path = join_url_path(upstream.base_path, target.path, request.query);
    const auto url = build_url(upstream, path);

    auto lease = impl_->pool.acquire();
    auto& slot = lease.get();
    RequestContext context;
    context.easy = slot.easy;
    context.timeouts = &timeouts_;
    context.max_response_body_size = max_response_body_size_;
    context.request_body_size = request.body.size();
    context.metrics = metrics_;
    context.inflight_budget = inflight_budget_;
    if (inflight_budget_) {
        auto memory = inflight_budget_->try_acquire(0);
        if (!memory) {
            throw ProxyError(
                503, "server_error", "inflight memory budget exhausted");
        }
        context.response_body_memory =
            std::make_shared<InflightMemoryBudget::Lease>(std::move(*memory));
        context.response.body_memory = context.response_body_memory;
    }
    context.cancellation = &cancellation;
    context.on_headers = &on_headers;
    context.on_chunk = &on_chunk;
    context.started = Clock::now();

    CurlHeaders request_headers;
    bool has_expect_header = false;
    for (const auto& [name, value] : filter_request_headers(request.headers)) {
        has_expect_header = has_expect_header || lower_copy(name) == "expect";
        request_headers.append(name + ": " + value);
    }
    if (!has_expect_header) {
        request_headers.append("Expect:");
    }

    char error_buffer[CURL_ERROR_SIZE]{};
    set_easy_option(slot.easy, CURLOPT_ERRORBUFFER, error_buffer);
    set_easy_option(slot.easy, CURLOPT_URL, url.c_str());
    set_easy_option(slot.easy, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    set_easy_option(slot.easy, CURLOPT_HTTPHEADER, request_headers.get());
    set_easy_option(slot.easy, CURLOPT_NOSIGNAL, 1L);
    set_easy_option(slot.easy, CURLOPT_FOLLOWLOCATION, 0L);
    set_easy_option(slot.easy, CURLOPT_PROTOCOLS_STR, "http,https");
    set_easy_option(slot.easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    set_easy_option(slot.easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
    set_easy_option(slot.easy, CURLOPT_HTTP09_ALLOWED, 0L);
    set_easy_option(slot.easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    set_easy_option(slot.easy, CURLOPT_TCP_KEEPALIVE, 1L);
    set_easy_option(slot.easy, CURLOPT_BUFFERSIZE, 64L * 1024L);
    const long combined_connect_timeout = static_cast<long>(std::min<long long>(
        static_cast<long long>(std::numeric_limits<long>::max()),
        static_cast<long long>(timeouts_.resolve_ms) + timeouts_.connect_ms));
    set_easy_option(slot.easy, CURLOPT_CONNECTTIMEOUT_MS, combined_connect_timeout);
    if (timeouts_.total_ms > 0) {
        set_easy_option(slot.easy, CURLOPT_TIMEOUT_MS, static_cast<long>(timeouts_.total_ms));
    }
    if (!request.body.empty() || request.method != "GET") {
        set_easy_option(slot.easy, CURLOPT_POSTFIELDS, request.body.data());
        set_easy_option(
            slot.easy,
            CURLOPT_POSTFIELDSIZE_LARGE,
            static_cast<curl_off_t>(request.body.size()));
    }
    set_easy_option(slot.easy, CURLOPT_SOCKOPTFUNCTION, socket_option_callback);
    set_easy_option(slot.easy, CURLOPT_SOCKOPTDATA, &context);
    set_easy_option(slot.easy, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    set_easy_option(slot.easy, CURLOPT_XFERINFODATA, &context);
    set_easy_option(slot.easy, CURLOPT_NOPROGRESS, 0L);
    set_easy_option(slot.easy, CURLOPT_HEADERFUNCTION, response_header_callback);
    set_easy_option(slot.easy, CURLOPT_HEADERDATA, &context);
    set_easy_option(slot.easy, CURLOPT_WRITEFUNCTION, response_body_callback);
    set_easy_option(slot.easy, CURLOPT_WRITEDATA, &context);

    const auto add_result = curl_multi_add_handle(slot.multi, slot.easy);
    if (add_result != CURLM_OK) {
        throw ProxyError(
            500,
            "server_error",
            "failed to start system libcurl request: "
                + std::string(curl_multi_strerror(add_result)));
    }
    slot.attached = true;
    bool added = true;
    const auto remove_handle = [&]() noexcept {
        if (!added) {
            return CURLM_OK;
        }
        const auto result = curl_multi_remove_handle(slot.multi, slot.easy);
        added = false;
        slot.attached = result != CURLM_OK;
        if (result != CURLM_OK) {
            lease.discard();
        }
        return result;
    };

    CURLcode transfer_result = CURLE_OK;
    CURLMcode remove_result = CURLM_OK;
    try {
        int running = 0;
        CURLMcode multi_result = CURLM_OK;
        do {
            multi_result = curl_multi_perform(slot.multi, &running);
        } while (multi_result == CURLM_CALL_MULTI_PERFORM);
        if (multi_result != CURLM_OK) {
            throw ProxyError(
                502,
                "upstream_error",
                "system libcurl perform failed: "
                    + std::string(curl_multi_strerror(multi_result)));
        }

        while (running != 0 && context.abort_reason == AbortReason::None) {
            context.abort_reason = deadline_abort(context);
            if (context.abort_reason != AbortReason::None) {
                break;
            }
            int descriptors = 0;
            multi_result = curl_multi_poll(
                slot.multi, nullptr, 0, kPollIntervalMs, &descriptors);
            if (multi_result != CURLM_OK) {
                throw ProxyError(
                    502,
                    "upstream_error",
                    "system libcurl poll failed: "
                        + std::string(curl_multi_strerror(multi_result)));
            }
            do {
                multi_result = curl_multi_perform(slot.multi, &running);
            } while (multi_result == CURLM_CALL_MULTI_PERFORM);
            if (multi_result != CURLM_OK) {
                throw ProxyError(
                    502,
                    "upstream_error",
                    "system libcurl perform failed: "
                        + std::string(curl_multi_strerror(multi_result)));
            }
        }

        if (context.abort_reason == AbortReason::None) {
            int messages = 0;
            CURLMsg* message = nullptr;
            while ((message = curl_multi_info_read(slot.multi, &messages)) != nullptr) {
                if (message->msg == CURLMSG_DONE && message->easy_handle == slot.easy) {
                    transfer_result = message->data.result;
                    break;
                }
            }
        }
        remove_result = remove_handle();
    } catch (...) {
        (void)remove_handle();
        throw;
    }

    if (remove_result != CURLM_OK) {
        throw ProxyError(
            502,
            "upstream_error",
            "failed to release system libcurl request: "
                + std::string(curl_multi_strerror(remove_result)));
    }
    if (context.callback_exception) {
        if (context.callback_exception_after_headers) {
            try {
                std::rethrow_exception(context.callback_exception);
            } catch (const InflightBudgetExceeded&) {
                throw ProxyError(
                    503, "server_error", "inflight memory budget exhausted");
            } catch (const ProxyError&) {
                throw;
            } catch (...) {
                throw ProxyError(
                    500, "server_error", "upstream stream callback failed");
            }
        }
        std::rethrow_exception(context.callback_exception);
    }

    if (cancellation.is_cancelled()
        && (context.abort_reason == AbortReason::None
            || context.abort_reason == AbortReason::CallbackStopped)) {
        context.abort_reason = AbortReason::Cancelled;
    }
    if (context.abort_reason == AbortReason::CallbackStopped) {
        if (metrics_) {
            metrics_->upstream_request_completed();
        }
        return std::move(context.response);
    }
    if (context.abort_reason != AbortReason::None) {
        if (context.abort_reason == AbortReason::Cancelled && metrics_) {
            metrics_->upstream_request_cancelled();
        } else if (is_timeout(context.abort_reason) && metrics_) {
            metrics_->upstream_timeout(timeout_phase(context.abort_reason));
        }
        throw abort_error(context.abort_reason);
    }
    if (transfer_result != CURLE_OK) {
        AbortReason curl_timeout_reason = AbortReason::None;
        if (transfer_result == CURLE_OPERATION_TIMEDOUT) {
            curl_timeout_reason = deadline_abort(context);
            if (!is_timeout(curl_timeout_reason)) {
                curl_timeout_reason = timeouts_.total_ms > 0
                    ? AbortReason::TotalTimeout
                    : !context.resolved ? AbortReason::ResolveTimeout
                    : !context.connected ? AbortReason::ConnectTimeout
                    : !context.send_completed ? AbortReason::SendTimeout
                    : !context.headers_complete ? AbortReason::ResponseHeaderTimeout
                    : context.streaming ? AbortReason::StreamIdleTimeout
                    : AbortReason::ResponseBodyTimeout;
            }
            if (metrics_) {
                metrics_->upstream_timeout(timeout_phase(curl_timeout_reason));
            }
            throw abort_error(curl_timeout_reason);
        }
        const std::string detail = error_buffer[0] != '\0'
            ? error_buffer
            : curl_easy_strerror(transfer_result);
        throw ProxyError(502, "upstream_error", "system libcurl request failed: " + detail);
    }
    if (!context.headers_complete) {
        throw ProxyError(502, "upstream_error", "upstream response did not contain HTTP headers");
    }
    if (context.response.status_code == 407) {
        throw ProxyError(
            502,
            "proxy_authentication_unsupported",
            "environment proxy requires authentication, which ccs-trans does not support");
    }
    if (metrics_) {
        metrics_->upstream_request_completed();
    }
    return std::move(context.response);
}

} // namespace ccs
