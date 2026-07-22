#pragma once

#include "gui_ipc/protocol_types.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace ccs::gui_ipc {

struct OutboundQueueStatus {
    std::size_t capacity = 0;
    std::size_t pending = 0;
    std::size_t peak_pending = 0;
    std::size_t rejected = 0;
    std::size_t coalesced = 0;
    bool closed = false;
};

struct OutboundItem {
    Envelope envelope;
    std::string coalescing_key;
};

class OutboundQueue {
public:
    explicit OutboundQueue(
        std::size_t capacity = kDefaultOutboundQueueCapacity);

    bool push(Envelope envelope, std::string coalescing_key = {});
    bool wait_pop(
        OutboundItem& item,
        std::chrono::milliseconds timeout);
    void close() noexcept;
    void reset();
    [[nodiscard]] OutboundQueueStatus status() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<OutboundItem> items_;
    std::size_t peak_pending_ = 0;
    std::size_t rejected_ = 0;
    std::size_t coalesced_ = 0;
    bool closed_ = false;
};

struct ServerSessionPolicy {
    std::string version;
    std::string source_commit;
    std::string instance_identity;
    std::string handshake_token;
    std::string session_id;
    std::optional<std::uint64_t> expected_process_id;
};

struct SessionValidation {
    bool accepted = false;
    ErrorCode error = ErrorCode::None;
    std::string detail;
};

class ServerSession {
public:
    explicit ServerSession(ServerSessionPolicy policy);

    SessionValidation accept_hello(
        const Envelope& envelope,
        const Hello& hello,
        std::uint64_t actual_process_id,
        std::uint64_t state_revision,
        HelloResult& result);
    SessionValidation accept_message(const Envelope& envelope);
    std::uint64_t next_server_sequence() noexcept;
    void disconnect() noexcept;

    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] std::uint64_t last_client_sequence() const noexcept;
    [[nodiscard]] const std::string& session_id() const noexcept;

private:
    ServerSessionPolicy policy_;
    std::uint64_t last_client_sequence_ = 0;
    std::uint64_t next_server_sequence_ = 1;
    bool authenticated_ = false;
    bool token_consumed_ = false;
};

class ClientStateTracker {
public:
    bool accept_snapshot(const Snapshot& snapshot, std::string& error);
    bool apply_delta(const StateDelta& delta, std::string& error);
    void disconnect() noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool snapshot_required() const noexcept;
    [[nodiscard]] const std::optional<Snapshot>& snapshot() const noexcept;

private:
    std::optional<Snapshot> snapshot_;
    bool connected_ = false;
    bool snapshot_required_ = true;
};

} // namespace ccs::gui_ipc
