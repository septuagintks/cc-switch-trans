#pragma once

#include "rules/rule_registry.hpp"

#include <memory>

namespace ccs {

std::shared_ptr<const RuleFactory> make_set_field_rule_factory();
std::shared_ptr<const RuleFactory> make_remove_field_rule_factory();

} // namespace ccs
