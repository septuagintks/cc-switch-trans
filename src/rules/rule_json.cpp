#include "rules/rule_json.hpp"

#include <stdexcept>
#include <utility>

namespace ccs {

namespace {

thread_local RuleJsonAllocationContext* current_context = nullptr;

} // namespace

RuleJsonAllocationContext::RuleJsonAllocationContext(
    std::shared_ptr<InflightMemoryBudget> budget) {
    if (!budget) {
        return;
    }
    memory_ = budget->try_acquire(0);
    if (!memory_) {
        throw InflightBudgetExceeded();
    }
}

bool RuleJsonAllocationContext::try_acquire(std::size_t bytes) noexcept {
    return !memory_ || memory_->try_grow(bytes);
}

void RuleJsonAllocationContext::release(std::size_t bytes) noexcept {
    if (memory_) {
        memory_->shrink(bytes);
    }
}

RuleJsonAllocationContext* RuleJsonAllocationContext::current() noexcept {
    return current_context;
}

RuleJsonAllocationContext* RuleJsonAllocationContext::set_current(
    RuleJsonAllocationContext* context) noexcept {
    return std::exchange(current_context, context);
}

RuleJsonAllocationScope::RuleJsonAllocationScope(
    RuleJsonAllocationContext* context) noexcept
    : previous_(RuleJsonAllocationContext::set_current(context)) {}

RuleJsonAllocationScope::~RuleJsonAllocationScope() {
    RuleJsonAllocationContext::set_current(previous_);
}

} // namespace ccs
