#pragma once

#include "config/profile_model.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ccs {

inline constexpr std::string_view kRulesTextSchema = "ccs-trans.rules/v1";

struct RulesTextError {
    std::string message;
    std::size_t line = 1;
    std::size_t column = 1;
    std::string rule_id;
    std::string rule_type;
    std::string option;
};

bool parse_rules_text(
    std::string_view content,
    const std::vector<StoredRule>& existing,
    std::vector<StoredRule>& rules,
    RulesTextError& error);
bool format_rules_text(
    const std::vector<StoredRule>& rules,
    std::string& content,
    RulesTextError& error);

} // namespace ccs
