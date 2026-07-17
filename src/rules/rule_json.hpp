#pragma once

#include "core/inflight_memory_budget.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ccs {

class RuleJsonAllocationContext {
public:
    explicit RuleJsonAllocationContext(
        std::shared_ptr<InflightMemoryBudget> budget);

    RuleJsonAllocationContext(const RuleJsonAllocationContext&) = delete;
    RuleJsonAllocationContext& operator=(const RuleJsonAllocationContext&) = delete;

    bool try_acquire(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;

    static RuleJsonAllocationContext* current() noexcept;
    static RuleJsonAllocationContext* set_current(
        RuleJsonAllocationContext* context) noexcept;

private:
    std::optional<InflightMemoryBudget::Lease> memory_;
};

class RuleJsonAllocationScope {
public:
    explicit RuleJsonAllocationScope(
        RuleJsonAllocationContext* context) noexcept;
    ~RuleJsonAllocationScope();

    RuleJsonAllocationScope(const RuleJsonAllocationScope&) = delete;
    RuleJsonAllocationScope& operator=(const RuleJsonAllocationScope&) = delete;

private:
    RuleJsonAllocationContext* previous_ = nullptr;
};

template <typename T>
class RuleJsonAllocator {
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    RuleJsonAllocator() noexcept
        : context_(RuleJsonAllocationContext::current()) {}

    explicit RuleJsonAllocator(RuleJsonAllocationContext* context) noexcept
        : context_(context) {}

    template <typename U>
    RuleJsonAllocator(const RuleJsonAllocator<U>& other) noexcept
        : context_(other.context()) {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        const auto bytes = count * sizeof(T);
        if (context_ && !context_->try_acquire(bytes)) {
            throw InflightBudgetExceeded();
        }
        try {
            return std::allocator<T>{}.allocate(count);
        } catch (...) {
            if (context_) {
                context_->release(bytes);
            }
            throw;
        }
    }

    void deallocate(T* pointer, std::size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
        if (context_) {
            context_->release(count * sizeof(T));
        }
    }

    RuleJsonAllocationContext* context() const noexcept {
        return context_;
    }

    template <typename U>
    bool operator==(const RuleJsonAllocator<U>& other) const noexcept {
        return context_ == other.context();
    }

    template <typename U>
    bool operator!=(const RuleJsonAllocator<U>& other) const noexcept {
        return !(*this == other);
    }

private:
    RuleJsonAllocationContext* context_ = nullptr;
};

using RuleString = std::basic_string<
    char,
    std::char_traits<char>,
    RuleJsonAllocator<char>>;
using RuleBinary = std::vector<std::uint8_t, RuleJsonAllocator<std::uint8_t>>;
using RuleJson = nlohmann::basic_json<
    std::map,
    std::vector,
    RuleString,
    bool,
    std::int64_t,
    std::uint64_t,
    double,
    RuleJsonAllocator,
    nlohmann::adl_serializer,
    RuleBinary>;

} // namespace ccs
