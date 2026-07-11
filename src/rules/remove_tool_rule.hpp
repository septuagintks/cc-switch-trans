#pragma once

#include "rules/rule_registry.hpp"

#include <memory>

namespace ccs {

std::shared_ptr<const RuleFactory> make_remove_tool_rule_factory();

} // namespace ccs
