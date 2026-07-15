#pragma once

#ifdef __APPLE__

#include "presentation/main_window_view_model.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ccs {

class MacMainWindow {
public:
    using LifecycleHandler = std::function<void(std::string_view)>;
    class Impl;

    explicit MacMainWindow(
        MainWindowViewModel& view_model,
        LifecycleHandler lifecycle_handler = {});
    ~MacMainWindow();

    MacMainWindow(const MacMainWindow&) = delete;
    MacMainWindow& operator=(const MacMainWindow&) = delete;

    bool show(MainWindowStateSnapshot state, std::string& error);
    void update(MainWindowStateSnapshot state);
    bool prepare_for_application_exit(std::function<void()> continuation);
    void destroy();

    bool exists() const noexcept;
    bool visible() const noexcept;
    static std::int64_t live_window_count() noexcept;
    static std::int64_t live_controller_count() noexcept;

    // Narrow automation entry point. The host calls this only in an explicit test environment.
    bool run_test_command(std::string_view command, std::string& error);

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace ccs

#endif
