#include "server/platform/local_socket.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace ccs::server_platform {

namespace {

constexpr int kBacklog = 64;
constexpr std::ptrdiff_t kInterrupted = -2;
volatile std::sig_atomic_t shutdown_requested = 0;
struct sigaction previous_sigint {};
struct sigaction previous_sigterm {};

int native_socket(SocketHandle socket) {
    return static_cast<int>(socket);
}

SocketHandle socket_handle(int socket) {
    return socket < 0 ? kInvalidSocket : static_cast<SocketHandle>(socket);
}

std::string socket_error(const char* operation, int error_code = errno) {
    return std::string(operation) + " failed: " + std::strerror(error_code)
        + " (" + std::to_string(error_code) + ")";
}

void shutdown_handler(int) {
    shutdown_requested = 1;
}

bool set_no_sigpipe(int socket) {
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    return setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) == 0;
#else
    (void)socket;
    return true;
#endif
}

} // namespace

SocketRuntime::SocketRuntime() = default;
SocketRuntime::~SocketRuntime() = default;

ShutdownSignalGuard::~ShutdownSignalGuard() {
    uninstall();
}

bool ShutdownSignalGuard::install(std::string& error) {
    if (installed_) {
        return true;
    }
    struct sigaction action {};
    action.sa_handler = shutdown_handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previous_sigint) != 0) {
        error = socket_error("sigaction(SIGINT)");
        return false;
    }
    if (sigaction(SIGTERM, &action, &previous_sigterm) != 0) {
        const int saved_error = errno;
        sigaction(SIGINT, &previous_sigint, nullptr);
        error = socket_error("sigaction(SIGTERM)", saved_error);
        return false;
    }
    installed_ = true;
    return true;
}

void ShutdownSignalGuard::uninstall() noexcept {
    if (!installed_) {
        return;
    }
    sigaction(SIGTERM, &previous_sigterm, nullptr);
    sigaction(SIGINT, &previous_sigint, nullptr);
    installed_ = false;
}

void reset_shutdown_signal() noexcept {
    shutdown_requested = 0;
}

bool shutdown_signal_requested() noexcept {
    return shutdown_requested != 0;
}

LocalListener::LocalListener(std::string host, std::uint16_t port)
    : host_(std::move(host))
    , port_(port) {}

LocalListener::~LocalListener() {
    close();
}

bool LocalListener::open(std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const auto port = std::to_string(port_);
    const int resolve_result = getaddrinfo(host_.c_str(), port.c_str(), &hints, &result);
    if (resolve_result != 0 || result == nullptr) {
        error = "failed to resolve listen address: " + host_ + ":" + port;
        if (resolve_result != 0) {
            error += ": " + std::string(gai_strerror(resolve_result));
        }
        return false;
    }

    int last_error = 0;
    for (addrinfo* address = result; address != nullptr; address = address->ai_next) {
        const int candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) {
            last_error = errno;
            continue;
        }
        const int reuse_address = 1;
        if (setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) != 0
            || !set_no_sigpipe(candidate)) {
            last_error = errno;
            ::close(candidate);
            continue;
        }
        if (::bind(candidate, address->ai_addr, address->ai_addrlen) == 0) {
            socket_ = socket_handle(candidate);
            break;
        }
        last_error = errno;
        ::close(candidate);
    }
    freeaddrinfo(result);

    if (socket_ == kInvalidSocket) {
        if (last_error == EADDRNOTAVAIL) {
            error = "failed to resolve listen address: " + host_ + ":" + port;
            return false;
        }
        error = "failed to bind listener " + host_ + ":" + port;
        if (last_error != 0) {
            error += ": " + std::string(std::strerror(last_error));
        }
        return false;
    }
    if (::listen(native_socket(socket_), kBacklog) != 0) {
        error = socket_error("listen");
        close();
        return false;
    }
    return true;
}

SocketWaitResult LocalListener::accept(
    AcceptedSocket& accepted,
    int timeout_ms,
    std::string& error) {
    accepted = {};
    pollfd entry{native_socket(socket_), POLLIN, 0};
    const int poll_result = ::poll(&entry, 1, timeout_ms);
    if (poll_result == 0) {
        return SocketWaitResult::Timeout;
    }
    if (poll_result < 0) {
        if (errno == EINTR) {
            return SocketWaitResult::Interrupted;
        }
        error = socket_error("listener poll");
        return SocketWaitResult::Failed;
    }
    if ((entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        error = "listener poll reported socket failure";
        return SocketWaitResult::Failed;
    }

    sockaddr_storage address{};
    socklen_t address_size = sizeof(address);
    const int client = ::accept(
        native_socket(socket_), reinterpret_cast<sockaddr*>(&address), &address_size);
    if (client < 0) {
        if (errno == EINTR) {
            return SocketWaitResult::Interrupted;
        }
        error = socket_error("listener accept");
        return SocketWaitResult::Failed;
    }
    if (!set_no_sigpipe(client)) {
        const int saved_error = errno;
        ::close(client);
        error = socket_error("setsockopt(SO_NOSIGPIPE)", saved_error);
        return SocketWaitResult::Failed;
    }

    char host[NI_MAXHOST]{};
    if (getnameinfo(
            reinterpret_cast<sockaddr*>(&address),
            address_size,
            host,
            sizeof(host),
            nullptr,
            0,
            NI_NUMERICHOST)
        == 0) {
        accepted.peer_ip = host;
    }
    accepted.handle = socket_handle(client);
    return SocketWaitResult::Ready;
}

std::uint16_t LocalListener::local_port() const noexcept {
    if (socket_ == kInvalidSocket) {
        return 0;
    }
    sockaddr_in address{};
    socklen_t address_size = sizeof(address);
    if (getsockname(
            native_socket(socket_), reinterpret_cast<sockaddr*>(&address), &address_size)
        != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

void LocalListener::close() noexcept {
    close_socket(socket_);
    socket_ = kInvalidSocket;
}

void close_socket(SocketHandle socket) noexcept {
    if (socket != kInvalidSocket) {
        ::close(native_socket(socket));
    }
}

void interrupt_and_close_socket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
    ::shutdown(native_socket(socket), SHUT_RDWR);
    close_socket(socket);
}

std::ptrdiff_t receive_socket(
    SocketHandle socket,
    char* buffer,
    std::size_t buffer_size,
    std::string& error) {
    const auto received = ::recv(native_socket(socket), buffer, buffer_size, 0);
    if (received >= 0) {
        return received;
    }
    if (errno == EINTR) {
        return kInterrupted;
    }
    error = socket_error("recv");
    return -1;
}

bool send_all(SocketHandle socket, const std::string& data, std::string& error) {
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto sent = ::send(native_socket(socket), cursor, remaining, flags);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            error = sent < 0 ? socket_error("send") : "send returned zero bytes";
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool poll_disconnected(
    const std::vector<SocketHandle>& sockets,
    int timeout_ms,
    std::vector<bool>& disconnected,
    std::string& error) {
    disconnected.assign(sockets.size(), false);
    if (sockets.empty()) {
        return true;
    }
    std::vector<pollfd> entries;
    entries.reserve(sockets.size());
    for (const auto socket : sockets) {
        entries.push_back(pollfd{native_socket(socket), POLLIN, 0});
    }
    const int result = ::poll(entries.data(), static_cast<nfds_t>(entries.size()), timeout_ms);
    if (result == 0 || (result < 0 && errno == EINTR)) {
        return true;
    }
    if (result < 0) {
        error = socket_error("client poll");
        return false;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto revents = entries[i].revents;
        bool closed = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (!closed && (revents & POLLIN) != 0) {
            char byte = 0;
            const auto received = ::recv(entries[i].fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            closed = received == 0
                || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
        }
        disconnected[i] = closed;
    }
    return true;
}

void shutdown_send_and_drain(SocketHandle socket, int drain_timeout_ms) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
    const int fd = native_socket(socket);
    ::shutdown(fd, SHUT_WR);
    timeval timeout{};
    timeout.tv_sec = drain_timeout_ms / 1000;
    timeout.tv_usec = (drain_timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char buffer[4096];
    while (::recv(fd, buffer, sizeof(buffer), 0) > 0) {
    }
    close_socket(socket);
}

} // namespace ccs::server_platform
