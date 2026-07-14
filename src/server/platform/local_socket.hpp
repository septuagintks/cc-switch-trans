#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace ccs::server_platform {

using SocketHandle = std::uintptr_t;
constexpr SocketHandle kInvalidSocket = std::numeric_limits<SocketHandle>::max();

struct AcceptedSocket {
    SocketHandle handle = kInvalidSocket;
    std::string peer_ip;
};

enum class SocketWaitResult {
    Ready,
    Timeout,
    Interrupted,
    Failed,
};

class SocketRuntime {
public:
    SocketRuntime();
    ~SocketRuntime();

    SocketRuntime(const SocketRuntime&) = delete;
    SocketRuntime& operator=(const SocketRuntime&) = delete;
};

class ShutdownSignalGuard {
public:
    ShutdownSignalGuard() = default;
    ~ShutdownSignalGuard();

    ShutdownSignalGuard(const ShutdownSignalGuard&) = delete;
    ShutdownSignalGuard& operator=(const ShutdownSignalGuard&) = delete;

    bool install(std::string& error);
    void uninstall() noexcept;

private:
    bool installed_ = false;
};

void reset_shutdown_signal() noexcept;
bool shutdown_signal_requested() noexcept;

class LocalListener {
public:
    LocalListener(std::string host, std::uint16_t port);
    ~LocalListener();

    LocalListener(const LocalListener&) = delete;
    LocalListener& operator=(const LocalListener&) = delete;

    bool open(std::string& error);
    SocketWaitResult accept(AcceptedSocket& accepted, int timeout_ms, std::string& error);
    std::uint16_t local_port() const noexcept;
    void close() noexcept;

private:
    std::string host_;
    std::uint16_t port_;
    SocketHandle socket_ = kInvalidSocket;
};

void close_socket(SocketHandle socket) noexcept;
void interrupt_and_close_socket(SocketHandle socket) noexcept;
std::ptrdiff_t receive_socket(
    SocketHandle socket,
    char* buffer,
    std::size_t buffer_size,
    std::string& error);
bool send_all(SocketHandle socket, const std::string& data, std::string& error);
bool poll_disconnected(
    const std::vector<SocketHandle>& sockets,
    int timeout_ms,
    std::vector<bool>& disconnected,
    std::string& error);
void shutdown_send_and_drain(SocketHandle socket, int drain_timeout_ms) noexcept;

} // namespace ccs::server_platform
