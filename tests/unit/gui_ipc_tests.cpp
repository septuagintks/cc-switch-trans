#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "gui_ipc/protocol_types.hpp"
#include "gui_ipc/session.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ccs::gui_ipc::FieldState sample_field() {
    return {
        "listener.port",
        "application",
        "unsigned_integer",
        true,
        1,
        65535,
        {},
        "field.listener.port",
        "service_restart",
        ccs::gui_ipc::FieldValue{std::uint64_t{15723}},
    };
}

ccs::gui_ipc::Snapshot sample_snapshot() {
    ccs::gui_ipc::Snapshot value;
    value.revision = 42;
    value.application = {"running", "127.0.0.1", 15723, {}, 0};
    value.profiles.push_back({
        7, "findcg", true, std::string{"responses"}, "ready", {}, 2, 1});
    value.application_fields.push_back(sample_field());
    value.selection = {std::string{"findcg"}, std::int64_t{7}};
    value.profile_editor = ccs::gui_ipc::ProfileEditor{
        7,
        "findcg",
        {ccs::gui_ipc::FieldState{
            "enabled", "profile", "boolean", true, {}, {}, {},
            "field.profile.enabled", "runtime_reload",
            ccs::gui_ipc::FieldValue{true}}},
    };
    value.rules_editor = ccs::gui_ipc::RulesEditor{
        7,
        "findcg",
        "{\n  \"schema_version\": \"ccs-trans.rules/v1\",\n  \"rules\": []\n}\n",
        ccs::gui_ipc::RulesDiagnostic{"test", 2, 3, "rule", "remove_tool", "tool"},
    };
    value.draft = {"dirty", false, 8, "sha256:opaque"};
    value.last_command = ccs::gui_ipc::CommandStatus{
        9,
        "save_profile",
        ccs::gui_ipc::ResultCode::Rejected,
        ccs::gui_ipc::ErrorCode::ValidationFailed,
        "findcg",
        "upstream.base-url",
        "invalid URL",
        std::string{"reload_draft"},
    };
    value.lightweight_mode = false;
    value.command_pending = true;
    return value;
}

template <typename Value, typename Serializer, typename Parser>
void require_round_trip(
    const Value& original,
    Serializer serializer,
    Parser parser,
    const std::string& label) {
    std::string content;
    std::string error;
    require(serializer(original, content, error), label + " serialize failed: " + error);
    Value parsed;
    require(parser(content, parsed, error), label + " parse failed: " + error);
    require(parsed == original, label + " round trip changed value");
}

void test_frame_codec() {
    std::vector<std::uint8_t> first;
    std::vector<std::uint8_t> second;
    ccs::gui_ipc::FrameError error;
    require(ccs::gui_ipc::encode_frame(R"({"hello":"world"})", first, error),
        "first frame encode failed");
    const std::string unicode_json =
        "{\"text\":\"\xe4\xbd\xa0\xe5\xa5\xbd\"}";
    require(ccs::gui_ipc::encode_frame(unicode_json, second, error),
        "Unicode frame encode failed");

    std::vector<std::uint8_t> joined = first;
    joined.insert(joined.end(), second.begin(), second.end());
    ccs::gui_ipc::FrameDecoder decoder;
    std::vector<std::string> frames;
    for (const auto byte : joined) {
        require(decoder.consume(std::span<const std::uint8_t>(&byte, 1), frames, error),
            std::string("fragmented frame failed: ") + ccs::gui_ipc::frame_error_name(error));
    }
    require(frames.size() == 2 && frames[0] == R"({"hello":"world"})",
        "fragmented/coalesced frames were not preserved");
    require(decoder.finish(error), "complete stream was reported incomplete");

    decoder.reset();
    frames.clear();
    require(decoder.consume(std::span<const std::uint8_t>(first.data(), 3), frames, error),
        "partial prefix was rejected early");
    require(!decoder.finish(error) && error == ccs::gui_ipc::FrameError::Incomplete,
        "partial prefix was not rejected at EOF");

    std::vector<std::uint8_t> oversized = {1, 0, 0, 1};
    decoder.reset();
    require(!decoder.consume(oversized, frames, error)
            && error == ccs::gui_ipc::FrameError::TooLarge,
        "oversized declared frame was accepted");

    const std::string invalid_utf8{"\xc0\x80", 2};
    std::vector<std::uint8_t> encoded;
    require(!ccs::gui_ipc::encode_frame(invalid_utf8, encoded, error)
            && error == ccs::gui_ipc::FrameError::InvalidUtf8,
        "overlong UTF-8 was accepted");

    const std::string large_payload(
        ccs::gui_ipc::kMaximumFrameBytes / 2 + 1024, 'x');
    std::vector<std::uint8_t> large_first;
    std::vector<std::uint8_t> large_second;
    require(ccs::gui_ipc::encode_frame(
                large_payload, large_first, error)
            && ccs::gui_ipc::encode_frame(
                large_payload, large_second, error),
        "legal large frames could not be encoded");
    large_first.insert(
        large_first.end(), large_second.begin(), large_second.end());
    require(large_first.size() > ccs::gui_ipc::kMaximumFrameBytes + 4,
        "large coalesced-frame fixture did not cross the decoder buffer limit");
    decoder.reset();
    frames.clear();
    require(decoder.consume(large_first, frames, error),
        "coalesced legal frames were treated as one oversized frame");
    require(frames.size() == 2
            && frames[0].size() == large_payload.size()
            && frames[1] == large_payload,
        "large coalesced frames were not decoded independently");
}

void test_envelope_codec() {
    std::string payload;
    std::string error;
    const ccs::gui_ipc::Hello hello{
        "0.8.0", "012345", "default", "one-time-token", 1234};
    require(ccs::gui_ipc::serialize_hello(hello, payload, error), error);
    ccs::gui_ipc::Envelope original;
    original.kind = ccs::gui_ipc::MessageKind::Hello;
    original.request_id = "request-1";
    original.source_commit = "012345";
    original.payload_json = payload;

    std::string content;
    require(ccs::gui_ipc::serialize_envelope(original, content, error), error);
    ccs::gui_ipc::Envelope parsed;
    require(ccs::gui_ipc::parse_envelope(content, parsed, error), error);
    require(parsed == original, "envelope round trip changed fields");

    const auto unknown = content.substr(0, content.size() - 1) + ",\"surprise\":1}";
    const auto before = parsed;
    require(!ccs::gui_ipc::parse_envelope(unknown, parsed, error)
            && error.find("unknown field") != std::string::npos,
        "unknown envelope field was accepted");
    require(parsed == before, "failed envelope parse changed its output");

    const auto duplicate = std::string{
        R"({"protocol":"ccs-trans.gui-ipc/v1","protocol":"ccs-trans.gui-ipc/v1",)"}
        + R"("kind":"hello","request_id":"r","session_id":"","sequence":0,)"
          R"("base_revision":"","source_commit":"x","payload":{}})";
    require(!ccs::gui_ipc::parse_envelope(duplicate, parsed, error)
            && error.find("duplicate") != std::string::npos,
        "duplicate envelope key was accepted");
}

void test_payload_codecs() {
    require_round_trip(
        ccs::gui_ipc::Hello{"0.8.0", "commit", "instance", "token", 99},
        ccs::gui_ipc::serialize_hello,
        ccs::gui_ipc::parse_hello,
        "hello");
    require_round_trip(
        ccs::gui_ipc::HelloResult{
            true, "0.8.0", "commit", "session", 4,
            ccs::gui_ipc::ErrorCode::None, {}},
        ccs::gui_ipc::serialize_hello_result,
        ccs::gui_ipc::parse_hello_result,
        "hello result");
    require_round_trip(
        ccs::gui_ipc::LaunchBootstrap{
            R"(\\.\pipe\ccs-trans.gui-ipc.v1.hash)",
            "0.8.0",
            "commit",
            "instance",
            "token",
            "session"},
        ccs::gui_ipc::serialize_launch_bootstrap,
        ccs::gui_ipc::parse_launch_bootstrap,
        "launch bootstrap");

    ccs::gui_ipc::Command command;
    command.command = ccs::gui_ipc::GuiCommand::SaveProfile;
    command.profile_id = "findcg";
    command.replacement_profile_id = "findcg-renamed";
    command.profile_key = 7;
    command.enabled = true;
    command.position = 3;
    command.field_edits = {
        {"id", ccs::gui_ipc::FieldValue{std::string{"findcg-renamed"}}},
        {"enabled", ccs::gui_ipc::FieldValue{true}},
        {"upstream.usage-path", std::nullopt},
    };
    command.text = "rules";
    command.expected_draft_revision = 10;
    command.expected_base_revision = "opaque";
    command.unsaved_decision = ccs::gui_ipc::UnsavedDecision::Discard;
    command.replace_existing_storage = true;
    command.replacement_confirmation = "REPLACE";
    require_round_trip(command, ccs::gui_ipc::serialize_command,
        ccs::gui_ipc::parse_command, "command");

    const auto snapshot = sample_snapshot();
    require_round_trip(snapshot, ccs::gui_ipc::serialize_snapshot,
        ccs::gui_ipc::parse_snapshot, "snapshot");

    ccs::gui_ipc::StateDelta delta;
    delta.from_revision = 40;
    delta.revision = 42;
    delta.application = snapshot.application;
    delta.profiles = snapshot.profiles;
    delta.selection = snapshot.selection;
    delta.profile_editor_changed = true;
    delta.profile_editor = snapshot.profile_editor;
    delta.rules_editor_changed = true;
    delta.rules_editor.reset();
    delta.draft = snapshot.draft;
    delta.last_command_changed = true;
    delta.last_command = snapshot.last_command;
    delta.lightweight_mode = snapshot.lightweight_mode;
    delta.command_pending = snapshot.command_pending;
    require_round_trip(delta, ccs::gui_ipc::serialize_state_delta,
        ccs::gui_ipc::parse_state_delta, "state delta");

    require_round_trip(*snapshot.last_command,
        ccs::gui_ipc::serialize_command_status,
        ccs::gui_ipc::parse_command_status,
        "command status");
    require_round_trip(
        ccs::gui_ipc::MaintenanceRequest{
            ccs::gui_ipc::MaintenanceCommand::WaitForRelease, 5000},
        ccs::gui_ipc::serialize_maintenance_request,
        ccs::gui_ipc::parse_maintenance_request,
        "maintenance request");
    require_round_trip(
        ccs::gui_ipc::MaintenanceResult{true, "0.8.0", "commit", "stopped", {}},
        ccs::gui_ipc::serialize_maintenance_result,
        ccs::gui_ipc::parse_maintenance_result,
        "maintenance result");
}

void test_malformed_payloads() {
    std::string error;
    ccs::gui_ipc::Hello hello;
    require(!ccs::gui_ipc::parse_hello(
                R"({"version":"x","source_commit":"x","instance_identity":"x",)"
                R"("handshake_token":"x","process_id":1,"extra":true})",
                hello,
                error)
            && error.find("unknown field") != std::string::npos,
        "unknown hello field was accepted");

    ccs::gui_ipc::StateDelta delta;
    require(!ccs::gui_ipc::parse_state_delta(
                R"({"from_revision":4,"revision":4,"command_pending":true})",
                delta,
                error)
            && error.find("advance revision") != std::string::npos,
        "non-advancing state delta was accepted");

    ccs::gui_ipc::Command command;
    const bool unknown_command_accepted = ccs::gui_ipc::parse_command(
                R"({"command":"unknown","profile_id":"","replacement_profile_id":"",)"
                R"("profile_key":null,"enabled":false,"position":0,"field_edits":[],)"
                R"("text":"","expected_draft_revision":null,"expected_base_revision":null,)"
                R"("unsaved_decision":null,"replace_existing_storage":false,)"
                R"("replacement_confirmation":""})",
                command,
                error);
    require(!unknown_command_accepted,
        "unknown GUI command was accepted");
    require(error.find("unknown GUI command") != std::string::npos,
        "unknown GUI command returned unstable error: " + error);
}

void test_session_and_queue_contracts() {
    ccs::gui_ipc::ServerSession session({
        "0.8.0", "commit", "instance", "secret", "session-1", 77});
    ccs::gui_ipc::Hello hello{"0.8.0", "commit", "instance", "secret", 77};
    std::string payload;
    std::string error;
    require(ccs::gui_ipc::serialize_hello(hello, payload, error), error);
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = ccs::gui_ipc::MessageKind::Hello;
    envelope.request_id = "hello-1";
    envelope.source_commit = "commit";
    envelope.payload_json = payload;
    ccs::gui_ipc::HelloResult hello_result;
    auto validation = session.accept_hello(envelope, hello, 77, 4, hello_result);
    require(validation.accepted && hello_result.accepted
            && hello_result.session_id == "session-1",
        "valid GUI handshake was rejected: " + validation.detail);

    ccs::gui_ipc::Envelope command;
    command.kind = ccs::gui_ipc::MessageKind::Command;
    command.request_id = "command-1";
    command.session_id = "session-1";
    command.sequence = 1;
    command.source_commit = "commit";
    require(session.accept_message(command).accepted,
        "first session command was rejected");
    require(!session.accept_message(command).accepted,
        "duplicate session sequence was accepted");
    command.sequence = 3;
    require(!session.accept_message(command).accepted,
        "session sequence gap was accepted");
    command.sequence = 2;
    require(session.accept_message(command).accepted,
        "sequence was advanced after a rejected gap");
    require(session.next_server_sequence() == 1
            && session.next_server_sequence() == 2,
        "server sequence is not monotonic");
    session.disconnect();
    require(!session.authenticated(), "disconnect retained authentication");
    require(!session.accept_hello(envelope, hello, 77, 4, hello_result).accepted,
        "one-time handshake token was reusable after disconnect");

    ccs::gui_ipc::OutboundQueue queue(2);
    ccs::gui_ipc::Envelope state = command;
    state.kind = ccs::gui_ipc::MessageKind::StateChanged;
    state.request_id = "state-1";
    require(queue.push(state, "latest-state"), "state queue rejected first item");
    state.request_id = "state-2";
    require(queue.push(state, "latest-state"), "state queue rejected coalesced item");
    ccs::gui_ipc::Envelope reliable = command;
    reliable.kind = ccs::gui_ipc::MessageKind::CommandStatus;
    reliable.result = ccs::gui_ipc::ResultCode::Succeeded;
    reliable.request_id = "result-1";
    require(queue.push(reliable), "queue rejected reliable command result");
    reliable.request_id = "result-2";
    require(!queue.push(reliable), "queue exceeded its fixed capacity");
    auto queue_status = queue.status();
    require(queue_status.pending == 2 && queue_status.coalesced == 1
            && queue_status.rejected == 1,
        "bounded queue counters are incorrect");
    ccs::gui_ipc::OutboundItem item;
    require(queue.wait_pop(item, std::chrono::milliseconds(1))
            && item.envelope.request_id == "state-2",
        "coalesced state did not retain the newest item in order");
    require(queue.wait_pop(item, std::chrono::milliseconds(1))
            && item.envelope.request_id == "result-1",
        "reliable result ordering changed");
    queue.close();
    require(!queue.push(command), "closed queue accepted another item");
}

void test_client_revision_tracker() {
    auto initial = sample_snapshot();
    ccs::gui_ipc::ClientStateTracker tracker;
    std::string error;
    require(tracker.accept_snapshot(initial, error), error);
    auto changed = initial;
    changed.revision = initial.revision + 2;
    changed.application.state = "reloading";
    changed.command_pending = false;
    ccs::gui_ipc::StateDelta delta;
    delta.from_revision = initial.revision;
    delta.revision = changed.revision;
    delta.application = changed.application;
    delta.command_pending = changed.command_pending;
    require(tracker.apply_delta(delta, error), error);
    require(tracker.snapshot() && tracker.snapshot()->application.state == "reloading"
            && tracker.snapshot()->profiles == initial.profiles,
        "state delta replaced unchanged snapshot sections");

    delta.from_revision = initial.revision;
    delta.revision = changed.revision + 1;
    require(!tracker.apply_delta(delta, error) && tracker.snapshot_required(),
        "revision gap did not require a fresh snapshot");
    tracker.disconnect();
    require(!tracker.connected() && tracker.snapshot_required(),
        "disconnect did not invalidate the remote state");
}

} // namespace

int main() {
    try {
        test_frame_codec();
        test_envelope_codec();
        test_payload_codecs();
        test_malformed_payloads();
        test_session_and_queue_contracts();
        test_client_revision_tracker();
        std::cout << "GUI IPC tests ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "GUI IPC tests failed: " << exception.what() << '\n';
        return 1;
    }
}
