#pragma once

#include "protocols/protocol_handler.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ccs {

class ProtocolRegistry {
public:
    bool register_handler(
        std::shared_ptr<const ProtocolHandler> handler,
        std::string& error);

    std::shared_ptr<const ProtocolHandler> find(std::string_view protocol_id) const;
    bool validate_profile(
        const std::shared_ptr<const ProtocolHandler>& handler,
        const std::string& profile_id,
        const ProfileDefinition& profile,
        std::string& error) const;
    bool is_known_specialized_rule(std::string_view rule_type) const;
    std::vector<std::string> protocol_ids() const;

private:
    std::unordered_map<std::string, std::shared_ptr<const ProtocolHandler>> handlers_;
    std::unordered_set<std::string> specialized_rule_types_;
};

std::shared_ptr<const ProtocolRegistry> builtin_protocol_registry();

} // namespace ccs
