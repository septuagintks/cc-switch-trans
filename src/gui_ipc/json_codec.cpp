#include "gui_ipc/json_codec.hpp"

#include "gui_ipc/json_codec_support.hpp"

#include <utility>

namespace ccs::gui_ipc {

using namespace json_detail;

bool serialize_envelope(
    const Envelope& envelope,
    std::string& content,
    std::string& error) {
    if (!validate_envelope(envelope, error)) return false;
    Json payload;
    if (!parse_json(envelope.payload_json, payload, error)
        || !payload.is_object()) {
        if (error.empty()) error = "IPC payload must be a JSON object";
        return false;
    }
    return serialize_with([&] {
        Json root = {
            {"protocol", envelope.protocol},
            {"kind", message_kind_name(envelope.kind)},
            {"request_id", envelope.request_id},
            {"session_id", envelope.session_id},
            {"sequence", envelope.sequence},
            {"base_revision", envelope.base_revision},
            {"source_commit", envelope.source_commit},
            {"payload", std::move(payload)},
        };
        if (envelope.result) {
            root["result"] = result_code_name(*envelope.result);
        }
        if (envelope.error_code) {
            root["error_code"] = error_code_name(*envelope.error_code);
        }
        return root;
    }, content, error);
}

bool parse_envelope(
    std::string_view content,
    Envelope& envelope,
    std::string& error) {
    Json root;
    if (!parse_json(content, root, error)
        || !check_keys(root,
            {"protocol", "kind", "request_id", "session_id", "sequence",
                "base_revision", "source_commit", "result", "error_code",
                "payload"},
            {"protocol", "kind", "request_id", "session_id", "sequence",
                "base_revision", "source_commit", "payload"},
            "$", error)) {
        return false;
    }
    Envelope parsed;
    std::string kind;
    if (!read_string(root.at("protocol"), parsed.protocol,
            "$.protocol", error)
        || !read_string(root.at("kind"), kind, "$.kind", error)
        || !parse_message_kind(kind, parsed.kind)
        || !read_string(root.at("request_id"), parsed.request_id,
            "$.request_id", error)
        || !read_string(root.at("session_id"), parsed.session_id,
            "$.session_id", error)
        || !read_integer(root.at("sequence"), parsed.sequence,
            "$.sequence", error)
        || !read_string(root.at("base_revision"), parsed.base_revision,
            "$.base_revision", error)
        || !read_string(root.at("source_commit"), parsed.source_commit,
            "$.source_commit", error)
        || !root.at("payload").is_object()) {
        if (error.empty()) {
            error = !parse_message_kind(kind, parsed.kind)
                ? "$.kind contains an unknown message kind"
                : "$.payload must be a JSON object";
        }
        return false;
    }
    parsed.payload_json = root.at("payload").dump();
    if (root.contains("result")) {
        std::string result;
        ResultCode result_code;
        if (!read_string(root.at("result"), result, "$.result", error)
            || !parse_result_code(result, result_code)) {
            if (error.empty()) {
                error = "$.result contains an unknown result code";
            }
            return false;
        }
        parsed.result = result_code;
    }
    if (root.contains("error_code")) {
        std::string name;
        ErrorCode code;
        if (!read_string(root.at("error_code"), name, "$.error_code", error)
            || !parse_error_code(name, code)) {
            if (error.empty()) {
                error = "$.error_code contains an unknown error code";
            }
            return false;
        }
        parsed.error_code = code;
    }
    if (!validate_envelope(parsed, error)) return false;
    envelope = std::move(parsed);
    return true;
}

bool serialize_hello(
    const Hello& value,
    std::string& content,
    std::string& error) {
    return serialize_with([&] { return Json{
        {"version", value.version},
        {"source_commit", value.source_commit},
        {"instance_identity", value.instance_identity},
        {"handshake_token", value.handshake_token},
        {"process_id", value.process_id},
    }; }, content, error);
}

bool parse_hello(
    std::string_view content,
    Hello& value,
    std::string& error) {
    return parse_payload(content, value, error,
        [](const Json& root, Hello& parsed, std::string_view path,
            std::string& failure) {
            return check_keys(root,
                       {"version", "source_commit", "instance_identity",
                           "handshake_token", "process_id"},
                       {"version", "source_commit", "instance_identity",
                           "handshake_token", "process_id"},
                       path, failure)
                && read_string(root.at("version"), parsed.version,
                    "$.version", failure)
                && read_string(root.at("source_commit"), parsed.source_commit,
                    "$.source_commit", failure)
                && read_string(root.at("instance_identity"),
                    parsed.instance_identity, "$.instance_identity", failure)
                && read_string(root.at("handshake_token"),
                    parsed.handshake_token, "$.handshake_token", failure)
                && read_integer(root.at("process_id"), parsed.process_id,
                    "$.process_id", failure);
        });
}

bool serialize_hello_result(
    const HelloResult& value,
    std::string& content,
    std::string& error) {
    return serialize_with([&] { return Json{
        {"accepted", value.accepted},
        {"version", value.version},
        {"source_commit", value.source_commit},
        {"session_id", value.session_id},
        {"state_revision", value.state_revision},
        {"error", error_code_name(value.error)},
        {"detail", value.detail},
    }; }, content, error);
}

bool parse_hello_result(
    std::string_view content,
    HelloResult& value,
    std::string& error) {
    return parse_payload(content, value, error,
        [](const Json& root, HelloResult& parsed, std::string_view path,
            std::string& failure) {
            std::string error_name;
            if (!check_keys(root,
                    {"accepted", "version", "source_commit", "session_id",
                        "state_revision", "error", "detail"},
                    {"accepted", "version", "source_commit", "session_id",
                        "state_revision", "error", "detail"},
                    path, failure)
                || !read_bool(root.at("accepted"), parsed.accepted,
                    "$.accepted", failure)
                || !read_string(root.at("version"), parsed.version,
                    "$.version", failure)
                || !read_string(root.at("source_commit"), parsed.source_commit,
                    "$.source_commit", failure)
                || !read_string(root.at("session_id"), parsed.session_id,
                    "$.session_id", failure)
                || !read_integer(root.at("state_revision"),
                    parsed.state_revision, "$.state_revision", failure)
                || !read_string(root.at("error"), error_name,
                    "$.error", failure)
                || !parse_error_code(error_name, parsed.error)
                || !read_string(root.at("detail"), parsed.detail,
                    "$.detail", failure)) {
                if (failure.empty()) {
                    failure = "$.error contains an unknown error code";
                }
                return false;
            }
            return true;
        });
}

bool serialize_launch_bootstrap(
    const LaunchBootstrap& value,
    std::string& content,
    std::string& error) {
    return serialize_with([&] { return Json{
        {"pipe_name", value.pipe_name_utf8},
        {"version", value.version},
        {"source_commit", value.source_commit},
        {"instance_identity", value.instance_identity},
        {"handshake_token", value.handshake_token},
        {"session_id", value.session_id},
    }; }, content, error);
}

bool parse_launch_bootstrap(
    std::string_view content,
    LaunchBootstrap& value,
    std::string& error) {
    return parse_payload(content, value, error,
        [](const Json& root, LaunchBootstrap& parsed, std::string_view path,
            std::string& failure) {
            return check_keys(root,
                       {"pipe_name", "version", "source_commit",
                           "instance_identity", "handshake_token", "session_id"},
                       {"pipe_name", "version", "source_commit",
                           "instance_identity", "handshake_token", "session_id"},
                       path, failure)
                && read_string(root.at("pipe_name"), parsed.pipe_name_utf8,
                    "$.pipe_name", failure)
                && read_string(root.at("version"), parsed.version,
                    "$.version", failure)
                && read_string(root.at("source_commit"), parsed.source_commit,
                    "$.source_commit", failure)
                && read_string(root.at("instance_identity"),
                    parsed.instance_identity, "$.instance_identity", failure)
                && read_string(root.at("handshake_token"),
                    parsed.handshake_token, "$.handshake_token", failure)
                && read_string(root.at("session_id"), parsed.session_id,
                    "$.session_id", failure);
        });
}

} // namespace ccs::gui_ipc
