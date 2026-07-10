#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace ccs {

class CancellationRegistration {
public:
    CancellationRegistration() = default;
    ~CancellationRegistration();

    CancellationRegistration(const CancellationRegistration&) = delete;
    CancellationRegistration& operator=(const CancellationRegistration&) = delete;
    CancellationRegistration(CancellationRegistration&& other) noexcept;
    CancellationRegistration& operator=(CancellationRegistration&& other) noexcept;

private:
    struct CallbackState {
        std::atomic_bool active{true};
        std::function<void()> callback;
    };

    explicit CancellationRegistration(std::shared_ptr<CallbackState> callback);
    void reset();

    std::shared_ptr<CallbackState> callback_;

    friend class CancellationToken;
    friend class CancellationSource;
};

class CancellationToken {
public:
    CancellationToken() = default;

    bool is_cancelled() const;
    CancellationRegistration on_cancel(std::function<void()> callback) const;

private:
    struct State {
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        std::vector<std::shared_ptr<CancellationRegistration::CallbackState>> callbacks;
    };

    explicit CancellationToken(std::shared_ptr<State> state);

    std::shared_ptr<State> state_;

    friend class CancellationSource;
};

class CancellationSource {
public:
    CancellationSource();

    CancellationToken token() const;
    bool cancel() const;

private:
    std::shared_ptr<CancellationToken::State> state_;
};

} // namespace ccs
