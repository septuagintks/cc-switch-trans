#pragma once

#include <filesystem>
#include <string>

namespace ccs {

enum class MacInstanceAcquireResult {
    Acquired,
    AlreadyRunning,
    Failed,
};

class MacInstanceCoordinator {
public:
    explicit MacInstanceCoordinator(std::filesystem::path lock_path);
    ~MacInstanceCoordinator();

    MacInstanceCoordinator(const MacInstanceCoordinator&) = delete;
    MacInstanceCoordinator& operator=(const MacInstanceCoordinator&) = delete;

    MacInstanceAcquireResult acquire(std::string& error);
    bool notify_existing(std::string& error) const;

private:
    std::filesystem::path lock_path_;
    int lock_file_ = -1;
};

inline constexpr const char* kMacShowMenuNotification =
    "com.septuagint.ccs-trans.show-menu";

} // namespace ccs
