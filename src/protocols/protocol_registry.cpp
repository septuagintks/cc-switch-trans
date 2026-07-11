#include "protocols/protocol_registry.hpp"

#include "protocols/chat_handler.hpp"
#include "protocols/messages_handler.hpp"
#include "protocols/responses_handler.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ccs {

namespace {

bool valid_method(const std::string& method) {
    if (method.empty() || method.size() > 32) {
        return false;
    }
    return std::all_of(method.begin(), method.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '-';
    });
}

} // namespace

bool ProtocolRegistry::register_handler(
    std::shared_ptr<const ProtocolHandler> handler,
    std::string& error) {
    error.clear();
    if (!handler) {
        error = "cannot register a null protocol handler";
        return false;
    }
    const auto& descriptor = handler->descriptor();
    if (!is_valid_protocol_id(descriptor.id)) {
        error = "invalid protocol id: " + descriptor.id;
        return false;
    }
    if (!valid_method(descriptor.request_method)) {
        error = "protocol " + descriptor.id + " has an invalid request method";
        return false;
    }
    if (descriptor.supports_usage && !valid_method(descriptor.usage_method)) {
        error = "protocol " + descriptor.id + " has an invalid Usage method";
        return false;
    }
    std::unordered_set<std::string> local_rules;
    for (const auto& rule_type : descriptor.specialized_rule_types) {
        if (!is_valid_rule_type(rule_type)) {
            error = "protocol " + descriptor.id + " declares an invalid specialized rule type: " + rule_type;
            return false;
        }
        if (!local_rules.emplace(rule_type).second) {
            error = "protocol " + descriptor.id + " declares duplicate specialized rule type: " + rule_type;
            return false;
        }
    }
    if (handlers_.count(descriptor.id) != 0) {
        error = "protocol handler already registered: " + descriptor.id;
        return false;
    }
    handlers_.emplace(descriptor.id, std::move(handler));
    specialized_rule_types_.insert(local_rules.begin(), local_rules.end());
    return true;
}

std::shared_ptr<const ProtocolHandler> ProtocolRegistry::find(std::string_view protocol_id) const {
    const auto handler = handlers_.find(std::string(protocol_id));
    return handler == handlers_.end() ? nullptr : handler->second;
}

bool ProtocolRegistry::validate_profile(
    const std::shared_ptr<const ProtocolHandler>& handler,
    const std::string& profile_id,
    const ProfileDefinition& profile,
    std::string& error) const {
    error.clear();
    if (!handler) {
        error = "profile " + profile_id + " has no protocol handler";
        return false;
    }
    const auto registered = find(handler->id());
    if (!registered || registered != handler) {
        error = "protocol handler is not registered in this registry: " + std::string(handler->id());
        return false;
    }
    if (!handler->validate_profile(profile_id, profile, error)) {
        return false;
    }
    for (const auto& rule : profile.rules) {
        if (!rule.enabled || !is_known_specialized_rule(rule.type)) {
            continue;
        }
        if (!handler->supports_specialized_rule(rule.type)) {
            error = "rule " + rule.id.value + " type " + rule.type
                + " is not supported by protocol " + std::string(handler->id());
            return false;
        }
    }
    return true;
}

bool ProtocolRegistry::is_known_specialized_rule(std::string_view rule_type) const {
    return specialized_rule_types_.count(std::string(rule_type)) != 0;
}

std::vector<std::string> ProtocolRegistry::protocol_ids() const {
    std::vector<std::string> ids;
    ids.reserve(handlers_.size());
    for (const auto& [id, handler] : handlers_) {
        (void)handler;
        ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::shared_ptr<const ProtocolRegistry> builtin_protocol_registry() {
    static const auto registry = []() {
        auto value = std::make_shared<ProtocolRegistry>();
        std::string error;
        if (!value->register_handler(std::make_shared<ResponsesHandler>(), error)
            || !value->register_handler(std::make_shared<ChatHandler>(), error)
            || !value->register_handler(std::make_shared<MessagesHandler>(), error)) {
            throw std::logic_error("failed to build protocol registry: " + error);
        }
        return std::shared_ptr<const ProtocolRegistry>(std::move(value));
    }();
    return registry;
}

} // namespace ccs
