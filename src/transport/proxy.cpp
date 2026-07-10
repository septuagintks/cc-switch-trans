#include "transport/proxy.hpp"

#include "core/url.hpp"
#include "transport/header_filter.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <functional>
#include <limits>
#include <sstream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace ccs {

namespace {

std::wstring widen(const std::string& value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

std::string narrow(const std::wstring& value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::string(value.begin(), value.end());
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

std::string trim(std::string value) {
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

std::wstring build_header_block(const Headers& headers) {
    std::ostringstream out;
    for (const auto& [name, value] : filter_request_headers(headers)) {
        out << name << ": " << value << "\r\n";
    }
    return widen(out.str());
}

Headers parse_raw_headers(const std::wstring& raw_headers) {
    Headers headers;
    std::istringstream stream(narrow(raw_headers));
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            continue;
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos) {
            headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
        }
    }
    return filter_response_headers(headers);
}

bool is_event_stream(const Headers& headers) {
    for (const auto& [name, value] : headers) {
        if (lower_copy(name) == "content-type" && lower_copy(value).find("text/event-stream") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string winhttp_error_message(const std::string& prefix) {
#ifdef _WIN32
    std::ostringstream out;
    out << prefix << " (winhttp error " << GetLastError() << ")";
    return out.str();
#else
    return prefix;
#endif
}

#ifdef _WIN32

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle)
        : handle_(handle) {}

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~WinHttpHandle() {
        reset();
    }

    HINTERNET get() const {
        return handle_;
    }

    explicit operator bool() const {
        return handle_ != nullptr;
    }

private:
    void reset() {
        if (handle_) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    HINTERNET handle_ = nullptr;
};

enum class RequestPhase {
    Sending,
    Resolving,
    Connecting,
    ResponseHeaders,
    StreamIdle,
    ResponseBody,
};

struct RequestContext {
    RuntimeMetrics* metrics = nullptr;
    std::atomic<RequestPhase> phase{RequestPhase::Sending};
};

class CancelableWinHttpHandle {
public:
    CancelableWinHttpHandle(HINTERNET handle, std::shared_ptr<RequestContext> context)
        : handle_(handle)
        , context_(std::move(context)) {}

    CancelableWinHttpHandle(const CancelableWinHttpHandle&) = delete;
    CancelableWinHttpHandle& operator=(const CancelableWinHttpHandle&) = delete;

    ~CancelableWinHttpHandle() {
        close();
    }

    HINTERNET get() const {
        return handle_.load(std::memory_order_acquire);
    }

    bool close() {
        const auto handle = handle_.exchange(nullptr, std::memory_order_acq_rel);
        if (handle == nullptr) {
            return false;
        }
        WinHttpCloseHandle(handle);
        return true;
    }

private:
    std::atomic<HINTERNET> handle_{nullptr};
    std::shared_ptr<RequestContext> context_;
};

class DeadlineTimer {
public:
    explicit DeadlineTimer(
        std::shared_ptr<CancelableWinHttpHandle> handle,
        std::function<void()> on_timeout = {})
        : state_(std::make_shared<State>()) {
        state_->handle = std::move(handle);
        state_->on_timeout = std::move(on_timeout);
        timer_ = CreateThreadpoolTimer(callback, state_.get(), nullptr);
        if (timer_ == nullptr) {
            throw ProxyError(500, "server_error", "failed to create request deadline timer");
        }
    }

    ~DeadlineTimer() {
        cancel();
        CloseThreadpoolTimer(timer_);
    }

    DeadlineTimer(const DeadlineTimer&) = delete;
    DeadlineTimer& operator=(const DeadlineTimer&) = delete;

    void arm(int timeout_ms) {
        cancel();
        state_->timed_out.store(false, std::memory_order_release);
        ULARGE_INTEGER due{};
        const auto relative = -static_cast<LONGLONG>(timeout_ms) * 10000LL;
        due.QuadPart = static_cast<ULONGLONG>(relative);
        FILETIME due_time{};
        due_time.dwLowDateTime = due.LowPart;
        due_time.dwHighDateTime = due.HighPart;
        SetThreadpoolTimer(timer_, &due_time, 0, 0);
    }

    void cancel() {
        if (timer_ != nullptr) {
            SetThreadpoolTimer(timer_, nullptr, 0, 0);
            WaitForThreadpoolTimerCallbacks(timer_, TRUE);
        }
    }

    bool timed_out() const {
        return state_->timed_out.load(std::memory_order_acquire);
    }

private:
    struct State {
        std::shared_ptr<CancelableWinHttpHandle> handle;
        std::function<void()> on_timeout;
        std::atomic_bool timed_out{false};
    };

    static VOID CALLBACK callback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER) {
        auto* state = static_cast<State*>(context);
        state->timed_out.store(true, std::memory_order_release);
        if (state->handle->close() && state->on_timeout) {
            state->on_timeout();
        }
    }

    std::shared_ptr<State> state_;
    PTP_TIMER timer_ = nullptr;
};

bool apply_timeouts(HINTERNET handle, const TimeoutConfig& timeouts, int receive_timeout_ms) {
    DWORD response_header_timeout = static_cast<DWORD>(timeouts.response_header_ms);
    return WinHttpSetTimeouts(
        handle,
        timeouts.resolve_ms,
        timeouts.connect_ms,
        timeouts.send_ms,
        receive_timeout_ms)
        && WinHttpSetOption(
            handle,
            WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
            &response_header_timeout,
            sizeof(response_header_timeout));
}

std::wstring query_raw_headers(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return {};
    }
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

int query_status_code(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to query upstream status"));
    }
    return static_cast<int>(status);
}

std::string read_body(
    const std::shared_ptr<CancelableWinHttpHandle>& request,
    std::size_t max_buffered_size,
    const std::function<bool(const std::string&)>& on_chunk,
    const std::shared_ptr<RuntimeMetrics>& metrics,
    const CancellationToken& cancellation,
    const std::string& timeout_type,
    const DeadlineTimer& total_deadline,
    int operation_timeout_ms) {
    std::string body;
    const auto timeout_phase = timeout_type == "upstream_stream_idle_timeout"
        ? UpstreamTimeoutPhase::StreamIdle
        : UpstreamTimeoutPhase::ResponseBody;
    DeadlineTimer operation_deadline(request, [metrics, timeout_phase]() {
        if (metrics) {
            metrics->upstream_timeout(timeout_phase);
        }
    });
    while (true) {
        DWORD available = 0;
        operation_deadline.arm(operation_timeout_ms);
        if (!WinHttpQueryDataAvailable(request->get(), &available)) {
            if (total_deadline.timed_out()) {
                throw ProxyError(504, "upstream_total_timeout", "upstream request total timeout exceeded");
            }
            if (operation_deadline.timed_out()) {
                throw ProxyError(504, timeout_type, "upstream response timed out");
            }
            if (cancellation.is_cancelled()) {
                throw ProxyError(499, "client_cancelled", "client disconnected");
            }
            if (GetLastError() == ERROR_WINHTTP_TIMEOUT) {
                if (metrics) {
                    metrics->upstream_timeout(timeout_phase);
                }
                throw ProxyError(504, timeout_type, "upstream response timed out");
            }
            throw ProxyError(502, "upstream_error", winhttp_error_message("failed to query upstream response data"));
        }
        if (available == 0) {
            break;
        }

        std::string chunk(available, '\0');
        DWORD read = 0;
        operation_deadline.arm(operation_timeout_ms);
        if (!WinHttpReadData(request->get(), chunk.data(), available, &read)) {
            if (total_deadline.timed_out()) {
                throw ProxyError(504, "upstream_total_timeout", "upstream request total timeout exceeded");
            }
            if (operation_deadline.timed_out()) {
                throw ProxyError(504, timeout_type, "upstream response timed out");
            }
            if (cancellation.is_cancelled()) {
                throw ProxyError(499, "client_cancelled", "client disconnected");
            }
            if (GetLastError() == ERROR_WINHTTP_TIMEOUT) {
                if (metrics) {
                    metrics->upstream_timeout(timeout_phase);
                }
                throw ProxyError(504, timeout_type, "upstream response timed out");
            }
            throw ProxyError(502, "upstream_error", winhttp_error_message("failed to read upstream response data"));
        }
        chunk.resize(read);
        if (metrics) {
            metrics->upstream_bytes_received(chunk.size());
        }
        if (on_chunk) {
            if (!on_chunk(chunk)) {
                if (cancellation.is_cancelled()) {
                    throw ProxyError(499, "client_cancelled", "client disconnected");
                }
                break;
            }
            continue;
        }
        if (chunk.size() > max_buffered_size - std::min(max_buffered_size, body.size())) {
            throw ProxyError(502, "upstream_response_too_large", "upstream response body too large");
        }
        body += chunk;
    }
    return body;
}

void CALLBACK winhttp_status_callback(
    HINTERNET,
    DWORD_PTR context,
    DWORD status,
    LPVOID,
    DWORD) {
    auto* request_context = reinterpret_cast<RequestContext*>(context);
    if (request_context == nullptr) {
        return;
    }
    auto* metrics = request_context->metrics;
    if (status == WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER) {
        request_context->phase.store(RequestPhase::Connecting, std::memory_order_release);
        if (metrics) {
            metrics->winhttp_connecting();
        }
    } else if (status == WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER) {
        request_context->phase.store(RequestPhase::Sending, std::memory_order_release);
        if (metrics) {
            metrics->winhttp_connected();
        }
    } else if (status == WINHTTP_CALLBACK_STATUS_RESOLVING_NAME) {
        request_context->phase.store(RequestPhase::Resolving, std::memory_order_release);
    } else if (status == WINHTTP_CALLBACK_STATUS_NAME_RESOLVED) {
        request_context->phase.store(RequestPhase::Sending, std::memory_order_release);
    } else if (status == WINHTTP_CALLBACK_STATUS_CONNECTION_CLOSED) {
        if (metrics) {
            metrics->winhttp_connection_closed();
        }
    }
}

std::string send_timeout_type(RequestPhase phase) {
    if (phase == RequestPhase::Resolving) {
        return "upstream_resolve_timeout";
    }
    if (phase == RequestPhase::Connecting) {
        return "upstream_connect_timeout";
    }
    return "upstream_send_timeout";
}

UpstreamTimeoutPhase send_timeout_phase(RequestPhase phase) {
    if (phase == RequestPhase::Resolving) {
        return UpstreamTimeoutPhase::Resolve;
    }
    if (phase == RequestPhase::Connecting) {
        return UpstreamTimeoutPhase::Connect;
    }
    return UpstreamTimeoutPhase::Send;
}

#endif

} // namespace

struct Proxy::Impl {
#ifdef _WIN32
    Impl(const TimeoutConfig& timeouts, const std::shared_ptr<RuntimeMetrics>& metrics)
        : session(WinHttpOpen(
              L"ccs-trans/0.4.0",
              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
              WINHTTP_NO_PROXY_NAME,
              WINHTTP_NO_PROXY_BYPASS,
              0)) {
        if (!session) {
            throw ProxyError(502, "upstream_error", winhttp_error_message("failed to open WinHTTP session"));
        }
        if (!apply_timeouts(session.get(), timeouts, timeouts.response_header_ms)) {
            throw ProxyError(502, "upstream_error", winhttp_error_message("failed to configure WinHTTP session timeouts"));
        }
        if (metrics) {
            const auto callback = WinHttpSetStatusCallback(
                session.get(),
                winhttp_status_callback,
                WINHTTP_CALLBACK_FLAG_RESOLVE_NAME
                    | WINHTTP_CALLBACK_FLAG_CONNECT_TO_SERVER
                    | WINHTTP_CALLBACK_FLAG_CLOSE_CONNECTION,
                0);
            if (callback == WINHTTP_INVALID_STATUS_CALLBACK) {
                throw ProxyError(502, "upstream_error", winhttp_error_message("failed to configure WinHTTP metrics callback"));
            }
        }
    }

    WinHttpHandle session;
#else
    Impl(const TimeoutConfig&, const std::shared_ptr<RuntimeMetrics>&) {}
#endif
};

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

Proxy::Proxy(
    TimeoutConfig timeouts,
    std::size_t max_response_body_size,
    std::shared_ptr<RuntimeMetrics> metrics)
    : timeouts_(timeouts)
    , max_response_body_size_(max_response_body_size)
    , metrics_(std::move(metrics))
    , impl_(std::make_unique<Impl>(timeouts_, metrics_)) {}

Proxy::~Proxy() = default;

HttpResponse Proxy::forward(
    const HttpRequest& request,
    const UpstreamTarget& target,
    const CancellationToken& cancellation) const {
    return forward_streaming(request, target, {}, {}, cancellation);
}

HttpResponse Proxy::forward_streaming(
    const HttpRequest& request,
    const UpstreamTarget& target,
    const HeaderCallback& on_headers,
    const ChunkCallback& on_chunk,
    const CancellationToken& cancellation) const {
#ifndef _WIN32
    (void)request;
    (void)target;
    (void)on_headers;
    (void)on_chunk;
    (void)cancellation;
    throw ProxyError(502, "upstream_error", "upstream forwarding is implemented with WinHTTP on Windows");
#else
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

    WinHttpHandle connection(WinHttpConnect(
        impl_->session.get(),
        widen(upstream.host).c_str(),
        static_cast<INTERNET_PORT>(upstream.port),
        0));
    if (!connection) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to connect to upstream"));
    }
    if (metrics_) {
        metrics_->upstream_connection_handle_created();
    }

    const DWORD flags = upstream.secure ? WINHTTP_FLAG_SECURE : 0;
    auto request_context = std::make_shared<RequestContext>();
    request_context->metrics = metrics_.get();
    auto upstream_request = std::make_shared<CancelableWinHttpHandle>(
        WinHttpOpenRequest(
            connection.get(),
            widen(request.method).c_str(),
            widen(path).c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags),
        request_context);
    if (upstream_request->get() == nullptr) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to create upstream request"));
    }
    if (metrics_) {
        metrics_->upstream_request_handle_created();
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(request_context.get());
        if (!WinHttpSetOption(
                upstream_request->get(),
                WINHTTP_OPTION_CONTEXT_VALUE,
                &context,
                sizeof(context))) {
            throw ProxyError(502, "upstream_error", winhttp_error_message("failed to configure WinHTTP request metrics"));
        }
    }
    auto cancellation_registration = cancellation.on_cancel([upstream_request, metrics = metrics_]() {
        if (upstream_request->close() && metrics) {
            metrics->upstream_request_cancelled();
        }
    });
    if (cancellation.is_cancelled()) {
        throw ProxyError(499, "client_cancelled", "client disconnected");
    }
    DeadlineTimer total_deadline(upstream_request, [metrics = metrics_]() {
        if (metrics) {
            metrics->upstream_timeout(UpstreamTimeoutPhase::Total);
        }
    });
    if (timeouts_.total_ms > 0) {
        total_deadline.arm(timeouts_.total_ms);
    }
    if (!apply_timeouts(upstream_request->get(), timeouts_, timeouts_.response_header_ms)) {
        if (cancellation.is_cancelled()) {
            throw ProxyError(499, "client_cancelled", "client disconnected");
        }
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to configure WinHTTP request timeouts"));
    }

    if (request.body.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
        throw ProxyError(413, "invalid_request_error", "request body too large for WinHTTP");
    }
    const auto header_block = build_header_block(request.headers);
    const DWORD body_size = static_cast<DWORD>(request.body.size());
    request_context->phase.store(RequestPhase::Sending, std::memory_order_release);
    if (!WinHttpSendRequest(
            upstream_request->get(),
            header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
            header_block.empty() ? 0 : static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA,
            0,
            body_size,
            0)) {
        if (total_deadline.timed_out()) {
            throw ProxyError(504, "upstream_total_timeout", "upstream request total timeout exceeded");
        }
        if (cancellation.is_cancelled()) {
            throw ProxyError(499, "client_cancelled", "client disconnected");
        }
        if (GetLastError() == ERROR_WINHTTP_TIMEOUT) {
            if (metrics_) {
                metrics_->upstream_timeout(send_timeout_phase(
                    request_context->phase.load(std::memory_order_acquire)));
            }
            throw ProxyError(
                504,
                send_timeout_type(request_context->phase.load(std::memory_order_acquire)),
                "upstream request send timed out");
        }
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to send upstream request"));
    }
    DWORD body_written = 0;
    if (body_size > 0
        && !WinHttpWriteData(
            upstream_request->get(),
            request.body.data(),
            body_size,
            &body_written)) {
        if (total_deadline.timed_out()) {
            throw ProxyError(504, "upstream_total_timeout", "upstream request total timeout exceeded");
        }
        if (cancellation.is_cancelled()) {
            throw ProxyError(499, "client_cancelled", "client disconnected");
        }
        if (GetLastError() == ERROR_WINHTTP_TIMEOUT) {
            if (metrics_) {
                metrics_->upstream_timeout(UpstreamTimeoutPhase::Send);
            }
            throw ProxyError(504, "upstream_send_timeout", "upstream request send timed out");
        }
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to write upstream request body"));
    }
    if (body_written != body_size) {
        throw ProxyError(502, "upstream_error", "incomplete upstream request body write");
    }
    if (metrics_) {
        metrics_->upstream_bytes_sent(body_written);
    }

    request_context->phase.store(RequestPhase::ResponseHeaders, std::memory_order_release);
    DeadlineTimer response_header_deadline(upstream_request, [metrics = metrics_]() {
        if (metrics) {
            metrics->upstream_timeout(UpstreamTimeoutPhase::ResponseHeader);
        }
    });
    response_header_deadline.arm(timeouts_.response_header_ms);
    if (!WinHttpReceiveResponse(upstream_request->get(), nullptr)) {
        if (total_deadline.timed_out()) {
            throw ProxyError(504, "upstream_total_timeout", "upstream request total timeout exceeded");
        }
        if (cancellation.is_cancelled()) {
            throw ProxyError(499, "client_cancelled", "client disconnected");
        }
        if (response_header_deadline.timed_out()) {
            throw ProxyError(504, "upstream_response_header_timeout", "upstream response header timed out");
        }
        if (GetLastError() == ERROR_WINHTTP_TIMEOUT) {
            if (metrics_) {
                metrics_->upstream_timeout(UpstreamTimeoutPhase::ResponseHeader);
            }
            throw ProxyError(504, "upstream_response_header_timeout", "upstream response header timed out");
        }
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to receive upstream response"));
    }
    if (response_header_deadline.timed_out()) {
        throw ProxyError(504, "upstream_response_header_timeout", "upstream response header timed out");
    }
    response_header_deadline.cancel();

    HttpResponse response;
    if (total_deadline.timed_out()) {
        throw ProxyError(504, "upstream_total_timeout", "upstream request total timeout exceeded");
    }
    response.status_code = query_status_code(upstream_request->get());
    response.headers = parse_raw_headers(query_raw_headers(upstream_request->get()));
    const bool streaming = is_event_stream(response.headers);
    request_context->phase.store(
        streaming ? RequestPhase::StreamIdle : RequestPhase::ResponseBody,
        std::memory_order_release);
    if (!apply_timeouts(
            upstream_request->get(),
            timeouts_,
            streaming ? timeouts_.stream_idle_ms : timeouts_.response_header_ms)) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to update WinHTTP receive timeout"));
    }
    if (streaming && on_headers && !on_headers(response)) {
        if (cancellation.is_cancelled()) {
            throw ProxyError(499, "client_cancelled", "client disconnected");
        }
        if (metrics_) {
            metrics_->upstream_request_completed();
        }
        return response;
    }
    response.body = read_body(
        upstream_request,
        max_response_body_size_,
        streaming ? on_chunk : ChunkCallback{},
        metrics_,
        cancellation,
        streaming ? "upstream_stream_idle_timeout" : "upstream_receive_timeout",
        total_deadline,
        streaming ? timeouts_.stream_idle_ms : timeouts_.response_header_ms);
    if (metrics_) {
        metrics_->upstream_request_completed();
    }
    return response;
#endif
}

} // namespace ccs
