#include "hosts/macos/startup_registration.hpp"

#import <ServiceManagement/ServiceManagement.h>

namespace ccs {

namespace {

std::string error_text(NSError* error, const std::string& fallback) {
    if (error == nil || error.localizedDescription == nil) {
        return fallback;
    }
    const char* text = error.localizedDescription.UTF8String;
    return text == nullptr ? fallback : std::string(text);
}

} // namespace

bool MacStartupRegistration::status(
    bool& enabled,
    bool& requires_approval,
    std::string& error) const {
    error.clear();
    enabled = false;
    requires_approval = false;
    switch (SMAppService.mainAppService.status) {
    case SMAppServiceStatusNotRegistered:
        return true;
    case SMAppServiceStatusEnabled:
        enabled = true;
        return true;
    case SMAppServiceStatusRequiresApproval:
        requires_approval = true;
        return true;
    case SMAppServiceStatusNotFound:
        error = "the main application login service was not found";
        return false;
    }
    error = "the main application login service returned an unknown status";
    return false;
}

bool MacStartupRegistration::set_registered(bool enabled, std::string& error) const {
    error.clear();
    bool currently_enabled = false;
    bool requires_approval = false;
    if (!status(currently_enabled, requires_approval, error)) {
        return false;
    }
    if (enabled && currently_enabled) {
        return true;
    }
    if (enabled && requires_approval) {
        error = "Launch at Login requires approval in System Settings";
        return false;
    }
    if (!enabled && !currently_enabled && !requires_approval) {
        return true;
    }

    NSError* native_error = nil;
    const BOOL succeeded = enabled
        ? [SMAppService.mainAppService registerAndReturnError:&native_error]
        : [SMAppService.mainAppService unregisterAndReturnError:&native_error];
    if (!succeeded) {
        error = error_text(
            native_error,
            enabled ? "failed to enable Launch at Login" : "failed to disable Launch at Login");
        return false;
    }
    if (enabled) {
        bool registered = false;
        if (!status(registered, requires_approval, error)) {
            return false;
        }
        if (requires_approval) {
            error = "Launch at Login requires approval in System Settings";
            return false;
        }
        if (!registered) {
            error = "Launch at Login registration did not become enabled";
            return false;
        }
    }
    return true;
}

} // namespace ccs
