#include "server/platform/local_socket.hpp"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <stdexcept>
#include <utility>

namespace ccs::server_platform {

namespace {

constexpr int kBacklog = 64;
constexpr std::ptrdiff_t kInterrupted = -2;
std::atomic_bool shutdown_requested{false};

SOCKET native_socket(SocketHandle socket) {
    return static_cast<SOCKET>(socket);
}

SocketHandle socket_handle(SOCKET socket) {
    return socket == INVALID_SOCKET ? kInvalidSocket : static_cast<SocketHandle>(socket);
}

std::string socket_error(const char* operation, int error_code = WSAGetLastError()) {
    return std::string(operation) + " failed with WinSock error " + std::to_string(error_code);
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        shutdown_requested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

} // namespace

SocketRuntime::SocketRuntime() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed with error " + std::to_string(result));
    }
}

SocketRuntime::~SocketRuntime() {
    WSACleanup();
}

ShutdownSignalGuard::~ShutdownSignalGuard() {
    uninstall();
}

bool ShutdownSignalGuard::install(std::string& error) {
    if (installed_) {
        return true;
    }
    if (SetConsoleCtrlHandler(console_handler, TRUE) == FALSE) {
        error = "failed to install the Windows console control handler";
        return false;
    }
    installed_ = true;
    return true;
}

void ShutdownSignalGuard::uninstall() noexcept {
    if (installed_) {
        SetConsoleCtrlHandler(console_handler, FALSE);
        installed_ = false;
    }
}

void reset_shutdown_signal() noexcept {
    shutdown_requested.store(false, std::memory_order_release);
}

bool shutdown_signal_requested() noexcept {
    return shutdown_requested.load(std::memory_order_acquire);
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
        return false;
    }

    for (addrinfo* address = result; address != nullptr; address = address->ai_next) {
        const SOCKET candidate = ::socket(
            address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) {
            continue;
        }
        const BOOL exclusive = TRUE;
        if (setsockopt(
                candidate,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive),
                sizeof(exclusive))
            == SOCKET_ERROR) {
            closesocket(candidate);
            continue;
        }
        if (::bind(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            socket_ = socket_handle(candidate);
            break;
        }
        closesocket(candidate);
    }
    freeaddrinfo(result);

    if (socket_ == kInvalidSocket) {
        error = "failed to bind listener " + host_ + ":" + port;
        return false;
    }
    if (::listen(native_socket(socket_), kBacklog) == SOCKET_ERROR) {
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
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(native_socket(socket_), &read_set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
    if (selected == 0) {
        return SocketWaitResult::Timeout;
    }
    if (selected == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEINTR) {
            return SocketWaitResult::Interrupted;
        }
        error = socket_error("listener wait");
        return SocketWaitResult::Failed;
    }

    sockaddr_storage address{};
    int address_size = sizeof(address);
    const SOCKET client = ::accept(
        native_socket(socket_), reinterpret_cast<sockaddr*>(&address), &address_size);
    if (client == INVALID_SOCKET) {
        if (WSAGetLastError() == WSAEINTR) {
            return SocketWaitResult::Interrupted;
        }
        error = socket_error("listener accept");
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
    int address_size = sizeof(address);
    if (getsockname(
            native_socket(socket_), reinterpret_cast<sockaddr*>(&address), &address_size)
        == SOCKET_ERROR) {
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
        closesocket(native_socket(socket));
    }
}

void interrupt_and_close_socket(SocketHandle socket) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
    shutdown(native_socket(socket), SD_BOTH);
    close_socket(socket);
}

std::ptrdiff_t receive_socket(
    SocketHandle socket,
    char* buffer,
    std::size_t buffer_size,
    std::string& error) {
    const int chunk_size = buffer_size > static_cast<std::size_t>(INT_MAX)
        ? INT_MAX
        : static_cast<int>(buffer_size);
    const int received = recv(native_socket(socket), buffer, chunk_size, 0);
    if (received != SOCKET_ERROR) {
        return received;
    }
    if (WSAGetLastError() == WSAEINTR) {
        return kInterrupted;
    }
    error = socket_error("recv");
    return -1;
}

bool send_all(SocketHandle socket, const std::string& data, std::string& error) {
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const int chunk_size = remaining > static_cast<std::size_t>(INT_MAX)
            ? INT_MAX
            : static_cast<int>(remaining);
        const int sent = send(native_socket(socket), cursor, chunk_size, 0);
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEINTR) {
            continue;
        }
        if (sent <= 0) {
            error = sent == SOCKET_ERROR ? socket_error("send") : "send returned zero bytes";
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
    std::vector<WSAPOLLFD> entries;
    entries.reserve(sockets.size());
    for (const auto socket : sockets) {
        entries.push_back(WSAPOLLFD{native_socket(socket), POLLRDNORM, 0});
    }
    const int result = WSAPoll(entries.data(), static_cast<ULONG>(entries.size()), timeout_ms);
    if (result == 0 || (result == SOCKET_ERROR && WSAGetLastError() == WSAEINTR)) {
        return true;
    }
    if (result == SOCKET_ERROR) {
        error = socket_error("client poll");
        return false;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto revents = entries[i].revents;
        bool closed = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
        if (!closed && (revents & POLLRDNORM) != 0) {
            char byte = 0;
            const int received = recv(entries[i].fd, &byte, 1, MSG_PEEK);
            closed = received == 0
                || (received == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK);
        }
        disconnected[i] = closed;
    }
    return true;
}

void shutdown_send_and_drain(SocketHandle socket, int drain_timeout_ms) noexcept {
    if (socket == kInvalidSocket) {
        return;
    }
    const SOCKET native = native_socket(socket);
    shutdown(native, SD_SEND);
    setsockopt(
        native,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&drain_timeout_ms),
        sizeof(drain_timeout_ms));
    char buffer[4096];
    while (recv(native, buffer, sizeof(buffer), 0) > 0) {
    }
    close_socket(socket);
}

} // namespace ccs::server_platform
