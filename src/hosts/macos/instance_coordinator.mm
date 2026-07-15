#include "hosts/macos/instance_coordinator.hpp"

#import <Foundation/Foundation.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace ccs {

MacInstanceCoordinator::MacInstanceCoordinator(std::filesystem::path lock_path)
    : lock_path_(std::move(lock_path)) {}

MacInstanceCoordinator::~MacInstanceCoordinator() {
    if (lock_file_ >= 0) {
        flock(lock_file_, LOCK_UN);
        close(lock_file_);
    }
}

MacInstanceAcquireResult MacInstanceCoordinator::acquire(std::string& error) {
    error.clear();
    if (lock_file_ >= 0) {
        return MacInstanceAcquireResult::Acquired;
    }
    lock_file_ = open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_file_ < 0) {
        error = "failed to open menu host instance lock: " + std::string(std::strerror(errno));
        return MacInstanceAcquireResult::Failed;
    }
    if (flock(lock_file_, LOCK_EX | LOCK_NB) == 0) {
        return MacInstanceAcquireResult::Acquired;
    }
    const int saved_error = errno;
    close(lock_file_);
    lock_file_ = -1;
    if (saved_error == EWOULDBLOCK) {
        return MacInstanceAcquireResult::AlreadyRunning;
    }
    error = "failed to acquire menu host instance lock: "
        + std::string(std::strerror(saved_error));
    return MacInstanceAcquireResult::Failed;
}

bool MacInstanceCoordinator::notify_existing(std::string& error) const {
    error.clear();
    NSString* name = [NSString stringWithUTF8String:kMacShowMenuNotification];
    NSString* object = [NSString stringWithUTF8String:lock_path_.c_str()];
    if (name == nil || object == nil) {
        error = "failed to construct the menu host notification identity";
        return false;
    }
    NSDictionary* user_info = nil;
    const char* automation = std::getenv("CCS_TRANS_MENU_TEST_AUTOMATION");
    const char* test_command = std::getenv("CCS_TRANS_MENU_TEST_COMMAND");
    if (automation != nullptr
        && std::string_view(automation) == "1"
        && test_command != nullptr) {
        NSString* command = [NSString stringWithUTF8String:test_command];
        if (command == nil) {
            error = "failed to construct the menu host test command";
            return false;
        }
        user_info = @{ @"test_command": command };
    }
    [NSDistributedNotificationCenter.defaultCenter
        postNotificationName:name
        object:object
        userInfo:user_info
        deliverImmediately:YES];
    return true;
}

} // namespace ccs
