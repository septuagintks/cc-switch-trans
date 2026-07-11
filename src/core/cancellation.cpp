#include "core/cancellation.hpp"

#include <algorithm>
#include <utility>

namespace ccs {

CancellationRegistration::CancellationRegistration(std::shared_ptr<CallbackState> callback)
    : callback_(std::move(callback)) {}

CancellationRegistration::~CancellationRegistration() {
    reset();
}

CancellationRegistration::CancellationRegistration(CancellationRegistration&& other) noexcept
    : callback_(std::move(other.callback_)) {}

CancellationRegistration& CancellationRegistration::operator=(CancellationRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        callback_ = std::move(other.callback_);
    }
    return *this;
}

void CancellationRegistration::reset() {
    if (callback_) {
        callback_->active.store(false, std::memory_order_release);
        callback_.reset();
    }
}

CancellationToken::CancellationToken(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

bool CancellationToken::is_cancelled() const {
    return state_ && state_->cancelled.load(std::memory_order_acquire);
}

CancellationRegistration CancellationToken::on_cancel(std::function<void()> callback) const {
    if (!state_) {
        return {};
    }

    auto callback_state = std::make_shared<CancellationRegistration::CallbackState>();
    callback_state->callback = std::move(callback);
    bool invoke_now = false;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->cancelled.load(std::memory_order_acquire)) {
            invoke_now = true;
        } else {
            state_->callbacks.erase(
                std::remove_if(
                    state_->callbacks.begin(),
                    state_->callbacks.end(),
                    [](const auto& registered) {
                        return !registered->active.load(std::memory_order_acquire);
                    }),
                state_->callbacks.end());
            state_->callbacks.push_back(callback_state);
        }
    }
    if (invoke_now) {
        if (callback_state->active.exchange(false, std::memory_order_acq_rel)) {
            callback_state->callback();
        }
        return {};
    }
    return CancellationRegistration(std::move(callback_state));
}

CancellationSource::CancellationSource()
    : state_(std::make_shared<CancellationToken::State>()) {}

CancellationToken CancellationSource::token() const {
    return CancellationToken(state_);
}

bool CancellationSource::cancel() const {
    if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }

    std::vector<std::shared_ptr<CancellationRegistration::CallbackState>> callbacks;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        callbacks.swap(state_->callbacks);
    }
    for (const auto& callback : callbacks) {
        if (callback->active.exchange(false, std::memory_order_acq_rel)) {
            callback->callback();
        }
    }
    return true;
}

} // namespace ccs
