#include "core/inflight_memory_budget.hpp"

#include "core/runtime_metrics.hpp"

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace ccs {

const char* InflightBudgetExceeded::what() const noexcept {
    return "inflight memory budget exhausted";
}

InflightMemoryBudget::Lease::Lease(
    std::shared_ptr<RuntimeMetrics> metrics,
    std::uint64_t bytes)
    : metrics_(std::move(metrics))
    , bytes_(bytes) {}

InflightMemoryBudget::Lease::~Lease() {
    reset();
}

InflightMemoryBudget::Lease::Lease(Lease&& other) noexcept
    : metrics_(std::move(other.metrics_))
    , bytes_(std::exchange(other.bytes_, 0)) {}

InflightMemoryBudget::Lease& InflightMemoryBudget::Lease::operator=(
    Lease&& other) noexcept {
    if (this != &other) {
        reset();
        metrics_ = std::move(other.metrics_);
        bytes_ = std::exchange(other.bytes_, 0);
    }
    return *this;
}

bool InflightMemoryBudget::Lease::valid() const noexcept {
    return metrics_ != nullptr;
}

std::uint64_t InflightMemoryBudget::Lease::bytes() const noexcept {
    return bytes_;
}

bool InflightMemoryBudget::Lease::try_grow(
    std::uint64_t additional_bytes) noexcept {
    if (!metrics_) {
        return false;
    }
    if (additional_bytes == 0) {
        return true;
    }
    if (!metrics_->try_acquire_inflight(additional_bytes)) {
        return false;
    }
    bytes_ += additional_bytes;
    return true;
}

void InflightMemoryBudget::Lease::shrink(std::uint64_t bytes) noexcept {
    assert(bytes <= bytes_);
    const auto released = std::min(bytes, bytes_);
    if (metrics_ && released != 0) {
        metrics_->release_inflight(released);
    }
    bytes_ -= released;
}

void InflightMemoryBudget::Lease::reset() noexcept {
    if (metrics_ && bytes_ != 0) {
        metrics_->release_inflight(bytes_);
    }
    bytes_ = 0;
    metrics_.reset();
}

InflightMemoryBudget::InflightMemoryBudget(
    std::uint64_t capacity,
    std::shared_ptr<RuntimeMetrics> metrics)
    : capacity_(capacity)
    , metrics_(std::move(metrics)) {
    if (capacity_ == 0) {
        throw std::invalid_argument("inflight memory budget must be positive");
    }
    if (!metrics_) {
        throw std::invalid_argument("inflight memory budget requires metrics");
    }
    if (!metrics_->configure_inflight_budget(capacity_)) {
        throw std::invalid_argument(
            "runtime metrics already use a different inflight memory budget");
    }
}

std::uint64_t InflightMemoryBudget::capacity() const noexcept {
    return capacity_;
}

std::optional<InflightMemoryBudget::Lease>
InflightMemoryBudget::try_acquire(std::uint64_t bytes) const noexcept {
    if (!metrics_->try_acquire_inflight(bytes)) {
        return std::nullopt;
    }
    return Lease(metrics_, bytes);
}

} // namespace ccs
