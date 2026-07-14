#include "server/platform/local_socket.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#endif

namespace {

using ccs::server_platform::SocketHandle;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

SocketHandle connect_loopback(std::uint16_t port) {
#ifdef _WIN32
    const SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET, "failed to create client socket");
#else
    const int client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client >= 0, "failed to create client socket");
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    require(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1,
        "failed to encode loopback address");
#ifdef _WIN32
    const int connect_result = ::connect(
        client, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
#else
    const int connect_result = ::connect(
        client,
        reinterpret_cast<const sockaddr*>(&address),
        static_cast<socklen_t>(sizeof(address)));
#endif
    if (connect_result != 0) {
        ccs::server_platform::close_socket(static_cast<SocketHandle>(client));
        throw std::runtime_error("failed to connect loopback client");
    }
    return static_cast<SocketHandle>(client);
}

#ifndef _WIN32
int count_open_file_descriptors() {
    int count = 0;
    for (int descriptor = 0; descriptor < 4096; ++descriptor) {
        errno = 0;
        if (fcntl(descriptor, F_GETFD) != -1 || errno != EBADF) {
            ++count;
        }
    }
    return count;
}
#endif

void force_reset_close(SocketHandle socket) {
    linger reset{};
    reset.l_onoff = 1;
    reset.l_linger = 0;
#ifdef _WIN32
    setsockopt(
        static_cast<SOCKET>(socket),
        SOL_SOCKET,
        SO_LINGER,
        reinterpret_cast<const char*>(&reset),
        sizeof(reset));
#else
    setsockopt(
        static_cast<int>(socket), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
#endif
    ccs::server_platform::close_socket(socket);
}

ccs::server_platform::AcceptedSocket accept_one(
    ccs::server_platform::LocalListener& listener) {
    ccs::server_platform::AcceptedSocket accepted;
    std::string error;
    const auto result = listener.accept(accepted, 1000, error);
    require(result == ccs::server_platform::SocketWaitResult::Ready,
        "listener did not accept a client: " + error);
    require(accepted.handle != ccs::server_platform::kInvalidSocket,
        "accepted client handle is invalid");
    require(accepted.peer_ip == "127.0.0.1", "accepted peer is not IPv4 loopback");
    return accepted;
}

void test_bind_accept_io_and_disconnect() {
    ccs::server_platform::SocketRuntime runtime;
#ifndef _WIN32
    const int baseline_file_descriptors = count_open_file_descriptors();
#endif
    ccs::server_platform::LocalListener listener("127.0.0.1", 0);
    std::string error;
    require(listener.open(error), "failed to open listener: " + error);
    const auto port = listener.local_port();
    require(port != 0, "listener did not expose its assigned port");

    ccs::server_platform::LocalListener conflict("127.0.0.1", port);
    std::string conflict_error;
    require(!conflict.open(conflict_error), "second listener bypassed the port conflict");

    ccs::server_platform::AcceptedSocket timed_out;
    require(listener.accept(timed_out, 1, error)
            == ccs::server_platform::SocketWaitResult::Timeout,
        "listener timeout did not return deterministically");

    const auto client = connect_loopback(port);
    const auto accepted = accept_one(listener);

    require(ccs::server_platform::send_all(client, "abc", error),
        "failed to send first request fragment: " + error);
    char receive_buffer[8]{};
    require(ccs::server_platform::receive_socket(
                accepted.handle, receive_buffer, 3, error)
            == 3
            && std::string(receive_buffer, 3) == "abc",
        "adapter did not preserve the first receive fragment");
    require(ccs::server_platform::send_all(client, "defg", error),
        "failed to send second request fragment: " + error);
    require(ccs::server_platform::receive_socket(
                accepted.handle, receive_buffer, 4, error)
            == 4
            && std::string(receive_buffer, 4) == "defg",
        "adapter did not preserve the second receive fragment");

    const std::string payload(2 * 1024 * 1024, 'x');
    std::string received;
    std::string reader_error;
    std::thread reader([&]() {
        std::vector<char> chunk(16384);
        while (received.size() < payload.size()) {
            const auto count = ccs::server_platform::receive_socket(
                client, chunk.data(), chunk.size(), reader_error);
            if (count <= 0) {
                return;
            }
            received.append(chunk.data(), static_cast<std::size_t>(count));
        }
    });
    require(ccs::server_platform::send_all(accepted.handle, payload, error),
        "failed to send a multi-write response: " + error);
    reader.join();
    require(reader_error.empty() && received == payload,
        "bounded send did not deliver the complete response");

    force_reset_close(client);
    std::vector<bool> disconnected;
    for (int attempt = 0; attempt < 20; ++attempt) {
        require(ccs::server_platform::poll_disconnected(
                    {accepted.handle}, 50, disconnected, error),
            "disconnect poll failed: " + error);
        if (!disconnected.empty() && disconnected.front()) {
            break;
        }
    }
    require(!disconnected.empty() && disconnected.front(),
        "client reset was not reported as disconnected");
    require(!ccs::server_platform::send_all(accepted.handle, "after-reset", error),
        "send unexpectedly succeeded after a client reset");

    ccs::server_platform::close_socket(accepted.handle);
    listener.close();
#ifndef _WIN32
    require(count_open_file_descriptors() == baseline_file_descriptors,
        "listener/client file descriptors were not fully reclaimed");
#endif
}

void test_interrupted_receive_and_shutdown() {
    ccs::server_platform::SocketRuntime runtime;
    ccs::server_platform::LocalListener listener("127.0.0.1", 0);
    std::string error;
    require(listener.open(error), "failed to open interrupt listener: " + error);
    const auto client = connect_loopback(listener.local_port());
    const auto accepted = accept_one(listener);

#ifndef _WIN32
    ccs::server_platform::ShutdownSignalGuard signal_guard;
    require(signal_guard.install(error), "failed to install POSIX signal handler: " + error);
    std::atomic<std::ptrdiff_t> interrupt_result{0};
    std::thread interrupted_reader([&]() {
        char byte = 0;
        std::string receive_error;
        interrupt_result.store(ccs::server_platform::receive_socket(
            accepted.handle, &byte, 1, receive_error));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    require(pthread_kill(interrupted_reader.native_handle(), SIGINT) == 0,
        "failed to interrupt the blocked receive");
    interrupted_reader.join();
    require(interrupt_result.load() == -2, "EINTR was not surfaced to the server loop");
    require(ccs::server_platform::shutdown_signal_requested(),
        "SIGINT did not update shutdown state");
    ccs::server_platform::reset_shutdown_signal();
#endif

    std::atomic<std::ptrdiff_t> shutdown_result{1};
    std::thread stopped_reader([&]() {
        char byte = 0;
        std::string receive_error;
        shutdown_result.store(ccs::server_platform::receive_socket(
            accepted.handle, &byte, 1, receive_error));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ccs::server_platform::interrupt_and_close_socket(accepted.handle);
    stopped_reader.join();
    require(shutdown_result.load() <= 0, "shutdown did not wake the blocked receive");

    ccs::server_platform::close_socket(client);
    listener.close();
}

} // namespace

int main() {
    try {
        test_bind_accept_io_and_disconnect();
        test_interrupted_receive_and_shutdown();
        std::cout << "local socket tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "local socket tests failed: " << ex.what() << "\n";
        return 1;
    }
}
