#pragma once

#include <cstdint>
#include <memory>
#include <optional>

namespace ccs {

class RuntimeMetrics;

inline constexpr std::uint64_t kDefaultInflightMemoryBudget =
    512ULL * 1024 * 1024;

class InflightMemoryBudget {
public:
    class Lease {
    public:
        Lease() = default;
        ~Lease();

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::uint64_t bytes() const noexcept;
        bool try_grow(std::uint64_t additional_bytes) noexcept;
        void shrink(std::uint64_t bytes) noexcept;
        void reset() noexcept;

    private:
        friend class InflightMemoryBudget;

        Lease(std::shared_ptr<RuntimeMetrics> metrics, std::uint64_t bytes);

        std::shared_ptr<RuntimeMetrics> metrics_;
        std::uint64_t bytes_ = 0;
    };

    InflightMemoryBudget(
        std::uint64_t capacity,
        std::shared_ptr<RuntimeMetrics> metrics);

    [[nodiscard]] std::uint64_t capacity() const noexcept;
    [[nodiscard]] std::optional<Lease> try_acquire(
        std::uint64_t bytes) const noexcept;

private:
    std::uint64_t capacity_ = 0;
    std::shared_ptr<RuntimeMetrics> metrics_;
};

} // namespace ccs
