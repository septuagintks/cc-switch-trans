#include "gui_ipc/session.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ccs::gui_ipc {

namespace {

std::size_t require_capacity(std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("GUI IPC outbound queue capacity must be positive");
    }
    return capacity;
}

bool constant_time_equal(std::string_view left, std::string_view right) noexcept {
    const auto maximum = std::max(left.size(), right.size());
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0; index < maximum; ++index) {
        const auto left_byte = index < left.size()
            ? static_cast<unsigned char>(left[index]) : 0U;
        const auto right_byte = index < right.size()
            ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= left_byte ^ right_byte;
    }
    return difference == 0;
}

SessionValidation reject(ErrorCode error, std::string detail) {
    return {false, error, std::move(detail)};
}

} // namespace

OutboundQueue::OutboundQueue(std::size_t capacity)
    : capacity_(require_capacity(capacity)) {}

bool OutboundQueue::push(Envelope envelope, std::string coalescing_key) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            ++rejected_;
            return false;
        }
        if (!coalescing_key.empty()) {
            const auto found = std::find_if(
                items_.rbegin(), items_.rend(), [&](const auto& item) {
                    return item.coalescing_key == coalescing_key;
                });
            if (found != items_.rend()) {
                found->envelope = std::move(envelope);
                ++coalesced_;
                return true;
            }
        }
        if (items_.size() >= capacity_) {
            ++rejected_;
            return false;
        }
        items_.push_back({std::move(envelope), std::move(coalescing_key)});
        peak_pending_ = std::max(peak_pending_, items_.size());
    }
    cv_.notify_one();
    return true;
}

bool OutboundQueue::wait_pop(
    OutboundItem& item,
    std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, timeout, [&] { return closed_ || !items_.empty(); })) {
        return false;
    }
    if (items_.empty()) {
        return false;
    }
    item = std::move(items_.front());
    items_.pop_front();
    return true;
}

void OutboundQueue::close() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cv_.notify_all();
}

void OutboundQueue::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
    closed_ = false;
}

OutboundQueueStatus OutboundQueue::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        capacity_, items_.size(), peak_pending_, rejected_, coalesced_, closed_,
    };
}

ServerSession::ServerSession(ServerSessionPolicy policy)
    : policy_(std::move(policy)) {}

SessionValidation ServerSession::accept_hello(
    const Envelope& envelope,
    const Hello& hello,
    std::uint64_t actual_process_id,
    std::uint64_t state_revision,
    HelloResult& result) {
    result = {
        false,
        policy_.version,
        policy_.source_commit,
        policy_.session_id,
        state_revision,
        ErrorCode::None,
        {},
    };
    std::string envelope_error;
    if (!validate_envelope(envelope, envelope_error)
        || envelope.kind != MessageKind::Hello) {
        result.error = ErrorCode::MalformedMessage;
        result.detail = envelope_error.empty()
            ? "the first GUI IPC message must be hello" : envelope_error;
        return reject(result.error, result.detail);
    }
    if (authenticated_ || token_consumed_) {
        result.error = ErrorCode::AuthenticationFailed;
        result.detail = "the one-time GUI handshake has already been consumed";
        return reject(result.error, result.detail);
    }
    if (hello.version != policy_.version) {
        result.error = ErrorCode::VersionMismatch;
        result.detail = "GUI and tray versions do not match";
        return reject(result.error, result.detail);
    }
    if (hello.source_commit != policy_.source_commit
        || envelope.source_commit != policy_.source_commit) {
        result.error = ErrorCode::SourceMismatch;
        result.detail = "GUI and tray source commits do not match";
        return reject(result.error, result.detail);
    }
    if (hello.instance_identity != policy_.instance_identity) {
        result.error = ErrorCode::AuthenticationFailed;
        result.detail = "GUI instance identity does not match the tray";
        return reject(result.error, result.detail);
    }
    if (!constant_time_equal(hello.handshake_token, policy_.handshake_token)) {
        result.error = ErrorCode::AuthenticationFailed;
        result.detail = "GUI handshake token is invalid";
        return reject(result.error, result.detail);
    }
    if (hello.process_id != actual_process_id
        || (policy_.expected_process_id
            && *policy_.expected_process_id != actual_process_id)) {
        result.error = ErrorCode::AuthenticationFailed;
        result.detail = "GUI process identity does not match the named-pipe client";
        return reject(result.error, result.detail);
    }
    authenticated_ = true;
    token_consumed_ = true;
    policy_.handshake_token.clear();
    last_client_sequence_ = 0;
    next_server_sequence_ = 1;
    result.accepted = true;
    return {true, ErrorCode::None, {}};
}

SessionValidation ServerSession::accept_message(const Envelope& envelope) {
    if (!authenticated_) {
        return reject(ErrorCode::AuthenticationFailed,
            "GUI IPC session has not completed hello");
    }
    std::string envelope_error;
    if (!validate_envelope(envelope, envelope_error)) {
        return reject(ErrorCode::MalformedMessage, std::move(envelope_error));
    }
    if (envelope.protocol != kProtocol) {
        return reject(ErrorCode::UnsupportedProtocol, "unsupported GUI IPC protocol");
    }
    if (envelope.source_commit != policy_.source_commit) {
        return reject(ErrorCode::SourceMismatch, "GUI IPC source commit changed during session");
    }
    if (envelope.session_id != policy_.session_id) {
        return reject(ErrorCode::SessionMismatch, "GUI IPC session_id does not match");
    }
    if (envelope.sequence != last_client_sequence_ + 1) {
        return reject(ErrorCode::SequenceRejected,
            envelope.sequence <= last_client_sequence_
                ? "GUI IPC sequence is duplicate or stale"
                : "GUI IPC sequence contains a gap");
    }
    last_client_sequence_ = envelope.sequence;
    return {true, ErrorCode::None, {}};
}

std::uint64_t ServerSession::next_server_sequence() noexcept {
    return next_server_sequence_++;
}

void ServerSession::disconnect() noexcept {
    authenticated_ = false;
    last_client_sequence_ = 0;
    next_server_sequence_ = 1;
}

bool ServerSession::authenticated() const noexcept {
    return authenticated_;
}

std::uint64_t ServerSession::last_client_sequence() const noexcept {
    return last_client_sequence_;
}

const std::string& ServerSession::session_id() const noexcept {
    return policy_.session_id;
}

bool ClientStateTracker::accept_snapshot(
    const Snapshot& snapshot,
    std::string& error) {
    error.clear();
    if (snapshot.revision == 0) {
        error = "GUI IPC snapshot revision must be positive";
        snapshot_required_ = true;
        return false;
    }
    snapshot_ = snapshot;
    connected_ = true;
    snapshot_required_ = false;
    return true;
}

bool ClientStateTracker::apply_delta(
    const StateDelta& delta,
    std::string& error) {
    error.clear();
    if (!snapshot_ || snapshot_required_) {
        error = "a complete GUI IPC snapshot is required before state deltas";
        snapshot_required_ = true;
        return false;
    }
    if (delta.from_revision != snapshot_->revision
        || delta.revision <= delta.from_revision
        || delta.empty()) {
        error = "GUI IPC state revision gap requires a new snapshot";
        snapshot_required_ = true;
        return false;
    }
    auto updated = *snapshot_;
    updated.revision = delta.revision;
    if (delta.application) updated.application = *delta.application;
    if (delta.profiles) updated.profiles = *delta.profiles;
    if (delta.application_fields) updated.application_fields = *delta.application_fields;
    if (delta.selection) updated.selection = *delta.selection;
    if (delta.profile_editor_changed) updated.profile_editor = delta.profile_editor;
    if (delta.rules_editor_changed) updated.rules_editor = delta.rules_editor;
    if (delta.draft) updated.draft = *delta.draft;
    if (delta.last_command_changed) updated.last_command = delta.last_command;
    if (delta.storage) updated.storage = *delta.storage;
    if (delta.lightweight_mode) updated.lightweight_mode = *delta.lightweight_mode;
    if (delta.command_pending) updated.command_pending = *delta.command_pending;
    snapshot_ = std::move(updated);
    return true;
}

void ClientStateTracker::disconnect() noexcept {
    connected_ = false;
    snapshot_required_ = true;
}

bool ClientStateTracker::connected() const noexcept {
    return connected_;
}

bool ClientStateTracker::snapshot_required() const noexcept {
    return snapshot_required_;
}

const std::optional<Snapshot>& ClientStateTracker::snapshot() const noexcept {
    return snapshot_;
}

} // namespace ccs::gui_ipc
