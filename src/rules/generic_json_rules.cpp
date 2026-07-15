#include "rules/generic_json_rules.hpp"

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ccs {

namespace {

constexpr std::size_t kMaxJsonPointerBytes = 4096;
constexpr std::size_t kMaxTraceTargetBytes = 256;

struct ParsedJsonPointer {
    std::string trace_target;
    std::vector<std::string> tokens;
};

std::string bounded_trace_target(const std::string& pointer) {
    if (pointer.size() <= kMaxTraceTargetBytes) {
        return pointer;
    }
    return "<json-pointer:" + std::to_string(pointer.size()) + "-bytes>";
}

bool decode_pointer_token(
    std::string_view encoded,
    std::string& decoded,
    std::string& error) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        if (encoded[index] != '~') {
            decoded.push_back(encoded[index]);
            continue;
        }
        if (index + 1 >= encoded.size()
            || (encoded[index + 1] != '0' && encoded[index + 1] != '1')) {
            error = "path contains an invalid RFC 6901 escape";
            return false;
        }
        decoded.push_back(encoded[index + 1] == '0' ? '~' : '/');
        ++index;
    }
    return true;
}

bool parse_json_pointer(
    const std::string& source,
    ParsedJsonPointer& pointer,
    std::string& error) {
    if (source.size() > kMaxJsonPointerBytes) {
        error = "path exceeds the 4096-byte JSON Pointer limit";
        return false;
    }
    if (!source.empty() && source.front() != '/') {
        error = "path must be an RFC 6901 JSON Pointer";
        return false;
    }

    ParsedJsonPointer candidate;
    candidate.trace_target = bounded_trace_target(source);
    if (!source.empty()) {
        std::size_t start = 1;
        while (true) {
            const auto end = source.find('/', start);
            const auto encoded = std::string_view(source).substr(
                start,
                end == std::string::npos ? std::string::npos : end - start);
            std::string decoded;
            if (!decode_pointer_token(encoded, decoded, error)) {
                return false;
            }
            candidate.tokens.push_back(std::move(decoded));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    pointer = std::move(candidate);
    return true;
}

bool validate_options(
    const RuleDefinition& definition,
    const std::unordered_set<std::string>& required,
    std::string& error) {
    for (const auto& [key, value] : definition.options) {
        (void)value;
        if (required.count(key) == 0) {
            error = "unsupported option: " + key;
            return false;
        }
    }
    for (const auto& key : required) {
        if (definition.options.count(key) == 0) {
            error = "missing required option: " + key;
            return false;
        }
    }
    return true;
}

bool require_json_protocol(const ProtocolHandler& protocol, std::string& error) {
    if (!protocol.descriptor().request_body_is_json) {
        error = "protocol " + std::string(protocol.id())
            + " does not declare a JSON request body";
        return false;
    }
    return true;
}

std::size_t parse_array_index(const std::string& token, std::size_t size) {
    if (token.empty()
        || token == "-"
        || (token.size() > 1 && token.front() == '0')) {
        throw RuleRuntimeError(
            "invalid_array_index",
            "JSON Pointer contains an invalid array index");
    }
    std::size_t value = 0;
    for (const char ch : token) {
        if (ch < '0' || ch > '9') {
            throw RuleRuntimeError(
                "invalid_array_index",
                "JSON Pointer contains an invalid array index");
        }
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            throw RuleRuntimeError(
                "invalid_array_index",
                "JSON Pointer array index is too large");
        }
        value = value * 10 + digit;
    }
    if (value >= size) {
        throw RuleRuntimeError(
            "array_index_out_of_bounds",
            "JSON Pointer array index is out of bounds");
    }
    return value;
}

nlohmann::json& resolve_child(nlohmann::json& parent, const std::string& token) {
    if (parent.is_object()) {
        const auto child = parent.find(token);
        if (child == parent.end()) {
            throw RuleRuntimeError(
                "target_missing",
                "JSON Pointer target does not exist");
        }
        return *child;
    }
    if (parent.is_array()) {
        return parent.at(parse_array_index(token, parent.size()));
    }
    throw RuleRuntimeError(
        "type_conflict",
        "JSON Pointer cannot traverse a scalar value");
}

class SetFieldRule final : public CompiledRule {
public:
    SetFieldRule(std::string id, ParsedJsonPointer pointer, nlohmann::json value)
        : CompiledRule(std::move(id), "set_field")
        , pointer_(std::move(pointer))
        , value_(std::move(value)) {}

    RuleApplyResult apply(nlohmann::json& body) const override {
        nlohmann::json* target = &body;
        for (const auto& token : pointer_.tokens) {
            target = &resolve_child(*target, token);
        }
        if (*target == value_) {
            return {true, false, "value_unchanged", {pointer_.trace_target, 0}};
        }
        *target = value_;
        return {true, true, "field_set", {pointer_.trace_target, 1}};
    }

private:
    ParsedJsonPointer pointer_;
    nlohmann::json value_;
};

class RemoveFieldRule final : public CompiledRule {
public:
    RemoveFieldRule(std::string id, ParsedJsonPointer pointer)
        : CompiledRule(std::move(id), "remove_field")
        , pointer_(std::move(pointer)) {}

    RuleApplyResult apply(nlohmann::json& body) const override {
        nlohmann::json* parent = &body;
        for (std::size_t index = 0; index + 1 < pointer_.tokens.size(); ++index) {
            parent = &resolve_child(*parent, pointer_.tokens[index]);
        }
        const auto& token = pointer_.tokens.back();
        if (parent->is_object()) {
            const auto target = parent->find(token);
            if (target == parent->end()) {
                throw RuleRuntimeError(
                    "target_missing",
                    "JSON Pointer target does not exist");
            }
            parent->erase(target);
        } else if (parent->is_array()) {
            const auto target_index = parse_array_index(token, parent->size());
            parent->erase(parent->begin() + static_cast<std::ptrdiff_t>(target_index));
        } else {
            throw RuleRuntimeError(
                "type_conflict",
                "JSON Pointer target parent is a scalar value");
        }
        return {true, true, "field_removed", {pointer_.trace_target, 1}};
    }

private:
    ParsedJsonPointer pointer_;
};

class SetFieldRuleFactory final : public RuleFactory {
public:
    std::string_view type() const noexcept override {
        return "set_field";
    }

    const RuleDescriptor& descriptor() const noexcept override {
        static const RuleDescriptor value{
            "set_field",
            "rule.set_field",
            false,
            {
                {"path", "rule.option.path", RuleOptionValueType::JsonPointer, true, 0},
                {"value", "rule.option.value", RuleOptionValueType::JsonValue, true, 1},
            },
        };
        return value;
    }

    bool compile(
        const RuleDefinition& definition,
        const ProtocolHandler& protocol,
        std::shared_ptr<const CompiledRule>& rule,
        std::string& error) const override {
        error.clear();
        if (!require_json_protocol(protocol, error)
            || !validate_options(definition, {"path", "value"}, error)) {
            return false;
        }
        const auto& path = definition.options.at("path");
        if (!path.is_string()) {
            error = "option path must be a string";
            return false;
        }
        ParsedJsonPointer pointer;
        if (!parse_json_pointer(path.get_ref<const std::string&>(), pointer, error)) {
            return false;
        }
        rule = std::make_shared<const SetFieldRule>(
            definition.id.value,
            std::move(pointer),
            definition.options.at("value"));
        return true;
    }
};

class RemoveFieldRuleFactory final : public RuleFactory {
public:
    std::string_view type() const noexcept override {
        return "remove_field";
    }

    const RuleDescriptor& descriptor() const noexcept override {
        static const RuleDescriptor value{
            "remove_field",
            "rule.remove_field",
            false,
            {
                {"path", "rule.option.path", RuleOptionValueType::JsonPointer, true, 0},
            },
        };
        return value;
    }

    bool compile(
        const RuleDefinition& definition,
        const ProtocolHandler& protocol,
        std::shared_ptr<const CompiledRule>& rule,
        std::string& error) const override {
        error.clear();
        if (!require_json_protocol(protocol, error)
            || !validate_options(definition, {"path"}, error)) {
            return false;
        }
        const auto& path = definition.options.at("path");
        if (!path.is_string()) {
            error = "option path must be a string";
            return false;
        }
        ParsedJsonPointer pointer;
        if (!parse_json_pointer(path.get_ref<const std::string&>(), pointer, error)) {
            return false;
        }
        if (pointer.tokens.empty()) {
            error = "remove_field cannot remove the JSON document root";
            return false;
        }
        rule = std::make_shared<const RemoveFieldRule>(
            definition.id.value,
            std::move(pointer));
        return true;
    }
};

} // namespace

std::shared_ptr<const RuleFactory> make_set_field_rule_factory() {
    return std::make_shared<const SetFieldRuleFactory>();
}

std::shared_ptr<const RuleFactory> make_remove_field_rule_factory() {
    return std::make_shared<const RemoveFieldRuleFactory>();
}

} // namespace ccs
