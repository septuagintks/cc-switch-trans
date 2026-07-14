#include "config/app_paths.hpp"
#include "hosts/macos/instance_coordinator.hpp"
#include "hosts/macos/menu_app.hpp"

#import <AppKit/AppKit.h>
#include <mach-o/dyld.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {

bool executable_path(std::filesystem::path& path, std::string& error) {
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        error = "failed to resolve the menu host executable path";
        return false;
    }
    std::error_code ec;
    path = std::filesystem::canonical(buffer.data(), ec);
    if (ec) {
        error = "failed to canonicalize the menu host executable path: " + ec.message();
        return false;
    }
    return true;
}

void show_error(const std::string& error) {
    NSString* detail = [NSString stringWithUTF8String:error.c_str()];
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"ccs-trans startup failed";
    alert.informativeText = detail == nil ? @"Unknown error" : detail;
    [alert runModal];
}

} // namespace

int main() {
    @autoreleasepool {
        NSApplication* application = NSApplication.sharedApplication;
        application.activationPolicy = NSApplicationActivationPolicyAccessory;

        ccs::AppPaths paths;
        std::filesystem::path executable;
        std::string error;
        if (!ccs::resolve_app_paths(paths, error)
            || !ccs::ensure_app_directories(paths, error)
            || !executable_path(executable, error)) {
            show_error(error);
            return 1;
        }

        ccs::MacInstanceCoordinator coordinator(paths.state_directory / "menu-host.lock");
        const auto acquired = coordinator.acquire(error);
        if (acquired == ccs::MacInstanceAcquireResult::AlreadyRunning) {
            if (!coordinator.notify_existing(error)) {
                show_error(error);
                return 1;
            }
            return 0;
        }
        if (acquired == ccs::MacInstanceAcquireResult::Failed) {
            show_error(error);
            return 1;
        }
        return ccs::run_menu_application(std::move(paths), std::move(executable));
    }
}
