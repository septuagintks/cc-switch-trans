#include "hosts/windows/gui_bridge/gui_outbound_channel.hpp"

#ifdef _WIN32

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/gui_bridge/gui_snapshot_builder.hpp"
#include "hosts/windows/windows_error.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ccs {

namespace {

std::size_t require_capacity(std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("GUI outbound channel capacity must be positive");
    }
    return capacity;
}

bool write_all(HANDLE pipe, const std::vector<std::uint8_t>& frame, std::string& error) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (event == nullptr) {
        error = windows_error_message(
            "failed to create GUI named-pipe write event", GetLastError());
        return false;
    }
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const auto remaining = frame.size() - offset;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(
            remaining, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        OVERLAPPED operation{};
        operation.hEvent = event;
        (void)ResetEvent(event);
        if (!WriteFile(pipe, frame.data() + offset, chunk, &written, &operation)) {
            const auto code = GetLastError();
            if (code != ERROR_IO_PENDING
                || WaitForSingleObject(event, INFINITE) != WAIT_OBJECT_0
                || !GetOverlappedResult(pipe, &operation, &written, FALSE)) {
                const auto failure = code == ERROR_IO_PENDING ? GetLastError() : code;
                error = windows_error_message(
                    "failed to write GUI named pipe", failure);
                CloseHandle(event);
                return false;
            }
        }
        if (written == 0) {
            error = "GUI named-pipe write returned zero bytes";
            CloseHandle(event);
            return false;
        }
        offset += written;
    }
    CloseHandle(event);
    return true;
}

} // namespace

GuiOutboundChannel::GuiOutboundChannel(
    HANDLE pipe,
    std::string session_id,
    std::string source_commit,
    SequenceProvider sequence_provider,
    FailureHandler failure_handler,
    std::size_t capacity)
    : pipe_(pipe)
    , session_id_(std::move(session_id))
    , source_commit_(std::move(source_commit))
    , sequence_provider_(std::move(sequence_provider))
    , failure_handler_(std::move(failure_handler))
    , capacity_(require_capacity(capacity)) {}

GuiOutboundChannel::~GuiOutboundChannel() {
    stop();
}

bool GuiOutboundChannel::start(std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (writer_.joinable()) {
        error = "GUI outbound writer was already started";
        return false;
    }
    try {
        writer_ = std::thread([this] { run(); });
    } catch (const std::exception& exception) {
        error = "failed to start GUI outbound writer: " + std::string(exception.what());
        return false;
    }
    return true;
}

bool GuiOutboundChannel::enqueue(gui_ipc::Envelope envelope) {
    return push_job(Job{JobKind::Envelope, std::move(envelope), std::nullopt}, true);
}

bool GuiOutboundChannel::enqueue_snapshot(
    std::string request_id,
    gui_ipc::Snapshot snapshot) {
    gui_ipc::Envelope envelope;
    envelope.kind = gui_ipc::MessageKind::Snapshot;
    envelope.request_id = std::move(request_id);
    envelope.base_revision = snapshot.draft.base_revision;
    return push_job(Job{
        JobKind::Snapshot, std::move(envelope), std::move(snapshot)}, true);
}

bool GuiOutboundChannel::publish_state(gui_ipc::Snapshot snapshot) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            ++rejected_;
            return false;
        }
        latest_state_ = std::move(snapshot);
        if (state_queued_) {
            ++coalesced_;
            return true;
        }
        if (jobs_.size() >= capacity_) {
            ++rejected_;
            return false;
        }
        jobs_.push_back(Job{JobKind::State, {}, std::nullopt});
        state_queued_ = true;
        peak_pending_ = std::max(peak_pending_, jobs_.size());
    }
    cv_.notify_one();
    return true;
}

bool GuiOutboundChannel::push_job(Job job, bool reliable) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || jobs_.size() >= capacity_) {
            ++rejected_;
            return false;
        }
        jobs_.push_back(std::move(job));
        peak_pending_ = std::max(peak_pending_, jobs_.size());
    }
    if (reliable) cv_.notify_one();
    return true;
}

void GuiOutboundChannel::close_after_drain() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        drain_ = true;
    }
    cv_.notify_all();
}

void GuiOutboundChannel::stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        drain_ = false;
        jobs_.clear();
        state_queued_ = false;
    }
    cv_.notify_all();
    if (writer_.joinable()) {
        (void)CancelSynchronousIo(
            reinterpret_cast<HANDLE>(writer_.native_handle()));
        writer_.join();
    }
}

void GuiOutboundChannel::join() noexcept {
    if (writer_.joinable()) writer_.join();
}

GuiOutboundStatus GuiOutboundChannel::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        capacity_, jobs_.size(), peak_pending_, rejected_, coalesced_,
        closed_, writer_failed_,
    };
}

bool GuiOutboundChannel::prepare_job(
    Job job,
    gui_ipc::Envelope& envelope,
    std::optional<gui_ipc::Snapshot>& baseline,
    std::string& error) {
    envelope = std::move(job.envelope);
    if (job.kind == JobKind::Envelope) return true;

    gui_ipc::Snapshot target;
    if (job.kind == JobKind::Snapshot) {
        target = std::move(*job.snapshot);
        if (!gui_ipc::serialize_snapshot(target, envelope.payload_json, error)) return false;
        baseline = std::move(target);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!latest_state_) return false;
        target = *latest_state_;
    }
    if (!sent_state_) {
        envelope.kind = gui_ipc::MessageKind::Snapshot;
        envelope.request_id = "state-snapshot";
        envelope.base_revision = target.draft.base_revision;
        if (!gui_ipc::serialize_snapshot(target, envelope.payload_json, error)) return false;
        baseline = std::move(target);
        return true;
    }
    const auto delta = build_gui_state_delta(*sent_state_, target);
    if (delta.empty()) return false;
    envelope.kind = gui_ipc::MessageKind::StateChanged;
    envelope.request_id = "state-changed";
    envelope.base_revision = target.draft.base_revision;
    if (!gui_ipc::serialize_state_delta(delta, envelope.payload_json, error)) return false;
    baseline = std::move(target);
    return true;
}

bool GuiOutboundChannel::write_envelope(
    gui_ipc::Envelope envelope,
    std::string& error) {
    envelope.session_id = session_id_;
    envelope.source_commit = source_commit_;
    if (envelope.kind != gui_ipc::MessageKind::HelloResult) {
        envelope.sequence = sequence_provider_();
    }
    std::string content;
    if (!gui_ipc::serialize_envelope(envelope, content, error)) return false;
    std::vector<std::uint8_t> frame;
    gui_ipc::FrameError frame_error;
    if (!gui_ipc::encode_frame(content, frame, frame_error)) {
        error = "failed to frame GUI IPC output: "
            + std::string(gui_ipc::frame_error_name(frame_error));
        return false;
    }
    return write_all(pipe_, frame, error);
}

void GuiOutboundChannel::run() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return closed_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                if (closed_) return;
                continue;
            }
            if (closed_ && !drain_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            if (job.kind == JobKind::State) state_queued_ = false;
        }

        gui_ipc::Envelope envelope;
        std::optional<gui_ipc::Snapshot> baseline;
        std::string error;
        if (!prepare_job(std::move(job), envelope, baseline, error)) {
            if (!error.empty()) {
                report_failure(std::move(error));
                return;
            }
            continue;
        }
        if (!write_envelope(std::move(envelope), error)) {
            report_failure(std::move(error));
            return;
        }
        if (baseline) sent_state_ = std::move(baseline);
    }
}

void GuiOutboundChannel::report_failure(std::string error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writer_failed_ = true;
        closed_ = true;
        drain_ = false;
        jobs_.clear();
        state_queued_ = false;
    }
    cv_.notify_all();
    if (failure_handler_) failure_handler_(std::move(error));
}

} // namespace ccs

#endif
