#pragma once

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/protocol_types.hpp"

#include "nlohmann/json.hpp"

#include <initializer_list>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ccs::gui_ipc::json_detail {

using Json = nlohmann::json;

inline bool check_keys(
    const Json& object,
    std::initializer_list<const char*> allowed,
    std::initializer_list<const char*> required,
    std::string_view path,
    std::string& error) {
    if (!object.is_object()) {
        error = std::string(path) + " must be a JSON object";
        return false;
    }
    std::unordered_set<std::string_view> accepted;
    for (const auto* key : allowed) accepted.emplace(key);
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!accepted.contains(it.key())) {
            error = std::string(path) + " contains unknown field: " + it.key();
            return false;
        }
    }
    for (const auto* key : required) {
        if (!object.contains(key)) {
            error = std::string(path) + " is missing required field: " + key;
            return false;
        }
    }
    return true;
}

inline bool parse_json(
    std::string_view content,
    Json& root,
    std::string& error) {
    error.clear();
    if (content.empty() || content.size() > kMaximumFrameBytes) {
        error = content.empty() ? "IPC JSON is empty" : "IPC JSON exceeds 16 MiB";
        return false;
    }
    if (!valid_utf8(content)) {
        error = "IPC JSON is not valid UTF-8";
        return false;
    }
    try {
        std::vector<std::unordered_set<std::string>> keys;
        std::string duplicate;
        const auto callback = [&](int, Json::parse_event_t event, Json& parsed) {
            if (event == Json::parse_event_t::object_start) {
                keys.emplace_back();
            } else if (event == Json::parse_event_t::key && !keys.empty()) {
                const auto& key = parsed.get_ref<const std::string&>();
                if (!keys.back().emplace(key).second && duplicate.empty()) {
                    duplicate = key;
                }
            } else if (event == Json::parse_event_t::object_end && !keys.empty()) {
                keys.pop_back();
            }
            return true;
        };
        root = Json::parse(content.begin(), content.end(), callback);
        if (!duplicate.empty()) {
            error = "IPC JSON contains duplicate object key: " + duplicate;
            return false;
        }
        return true;
    } catch (const Json::exception& exception) {
        error = "failed to parse IPC JSON: " + std::string(exception.what());
        return false;
    }
}

template <typename Builder>
bool serialize_with(
    Builder&& builder,
    std::string& content,
    std::string& error) {
    content.clear();
    error.clear();
    try {
        const Json root = builder();
        content = root.dump();
        if (content.size() > kMaximumFrameBytes) {
            content.clear();
            error = "serialized IPC JSON exceeds 16 MiB";
            return false;
        }
        return true;
    } catch (const Json::exception& exception) {
        error = "failed to serialize IPC JSON: " + std::string(exception.what());
        return false;
    }
}

template <typename Integer>
bool read_integer(
    const Json& value,
    Integer& result,
    std::string_view path,
    std::string& error) {
    static_assert(std::is_integral_v<Integer>);
    if constexpr (std::is_signed_v<Integer>) {
        if (!value.is_number_integer()) {
            error = std::string(path) + " must be a JSON integer";
            return false;
        }
        const auto parsed = value.get<std::int64_t>();
        if (parsed < static_cast<std::int64_t>(
                std::numeric_limits<Integer>::min())
            || parsed > static_cast<std::int64_t>(
                std::numeric_limits<Integer>::max())) {
            error = std::string(path) + " is outside the supported integer range";
            return false;
        }
        result = static_cast<Integer>(parsed);
    } else {
        if (!value.is_number_unsigned()
            && !(value.is_number_integer() && value.get<std::int64_t>() >= 0)) {
            error = std::string(path) + " must be a non-negative JSON integer";
            return false;
        }
        const auto parsed = value.get<std::uint64_t>();
        if (parsed > static_cast<std::uint64_t>(
                std::numeric_limits<Integer>::max())) {
            error = std::string(path) + " is outside the supported integer range";
            return false;
        }
        result = static_cast<Integer>(parsed);
    }
    return true;
}

inline bool read_string(
    const Json& value,
    std::string& result,
    std::string_view path,
    std::string& error) {
    if (!value.is_string()) {
        error = std::string(path) + " must be a JSON string";
        return false;
    }
    result = value.get<std::string>();
    return true;
}

inline bool read_bool(
    const Json& value,
    bool& result,
    std::string_view path,
    std::string& error) {
    if (!value.is_boolean()) {
        error = std::string(path) + " must be a JSON boolean";
        return false;
    }
    result = value.get<bool>();
    return true;
}

inline Json field_value_json(const FieldValue& value) {
    return std::visit([](const auto& item) -> Json { return item; }, value);
}

inline bool parse_field_value(
    const Json& value,
    FieldValue& parsed,
    std::string_view path,
    std::string& error) {
    if (value.is_string()) {
        parsed = value.get<std::string>();
        return true;
    }
    if (value.is_boolean()) {
        parsed = value.get<bool>();
        return true;
    }
    if (value.is_number_unsigned()
        || (value.is_number_integer() && value.get<std::int64_t>() >= 0)) {
        parsed = value.get<std::uint64_t>();
        return true;
    }
    error = std::string(path)
        + " must be a string, unsigned integer, or boolean";
    return false;
}

template <typename Value, typename Parser>
bool parse_payload(
    std::string_view content,
    Value& value,
    std::string& error,
    Parser&& parser) {
    Json root;
    if (!parse_json(content, root, error)) return false;
    Value parsed;
    if (!parser(root, parsed, "$", error)) return false;
    value = std::move(parsed);
    return true;
}

} // namespace ccs::gui_ipc::json_detail
