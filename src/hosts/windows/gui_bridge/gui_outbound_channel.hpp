#pragma once

#ifdef _WIN32

#include "gui_ipc/protocol_types.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ccs {

struct GuiOutboundStatus {
    std::size_t capacity = 0;
    std::size_t pending = 0;
    std::size_t peak_pending = 0;
    std::size_t rejected = 0;
    std::size_t coalesced = 0;
    bool closed = false;
    bool writer_failed = false;
};

class GuiOutboundChannel {
public:
    using SequenceProvider = std::function<std::uint64_t()>;
    using FailureHandler = std::function<void(std::string)>;

    GuiOutboundChannel(
        HANDLE pipe,
        std::string session_id,
        std::string source_commit,
        SequenceProvider sequence_provider,
        FailureHandler failure_handler,
        std::size_t capacity);
    ~GuiOutboundChannel();

    GuiOutboundChannel(const GuiOutboundChannel&) = delete;
    GuiOutboundChannel& operator=(const GuiOutboundChannel&) = delete;

    bool start(std::string& error);
    bool enqueue(gui_ipc::Envelope envelope);
    bool enqueue_snapshot(std::string request_id, gui_ipc::Snapshot snapshot);
    bool publish_state(gui_ipc::Snapshot snapshot);
    void close_after_drain() noexcept;
    void join() noexcept;
    void stop() noexcept;
    [[nodiscard]] GuiOutboundStatus status() const;

private:
    enum class JobKind { Envelope, Snapshot, State };
    struct Job {
        JobKind kind = JobKind::Envelope;
        gui_ipc::Envelope envelope;
        std::optional<gui_ipc::Snapshot> snapshot;
    };

    bool push_job(Job job, bool reliable);
    bool prepare_job(
        Job job,
        gui_ipc::Envelope& envelope,
        std::optional<gui_ipc::Snapshot>& baseline,
        std::string& error);
    bool write_envelope(gui_ipc::Envelope envelope, std::string& error);
    void run();
    void report_failure(std::string error);

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::string session_id_;
    std::string source_commit_;
    SequenceProvider sequence_provider_;
    FailureHandler failure_handler_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::optional<gui_ipc::Snapshot> latest_state_;
    std::optional<gui_ipc::Snapshot> sent_state_;
    std::size_t peak_pending_ = 0;
    std::size_t rejected_ = 0;
    std::size_t coalesced_ = 0;
    bool state_queued_ = false;
    bool closed_ = false;
    bool drain_ = false;
    bool writer_failed_ = false;
    std::thread writer_;
};

} // namespace ccs

#endif
