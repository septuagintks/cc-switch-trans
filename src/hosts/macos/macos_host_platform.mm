#include "hosts/macos/macos_host_platform.hpp"

#import <AppKit/AppKit.h>

namespace ccs {

namespace {

bool open_path(const std::filesystem::path& path, std::string& error) {
    error.clear();
    const auto utf8 = path.u8string();
    NSString* value = [[NSString alloc] initWithBytes:utf8.data()
        length:utf8.size()
        encoding:NSUTF8StringEncoding];
    if (value == nil) {
        error = "failed to convert path to UTF-8";
        return false;
    }
    NSURL* url = [NSURL fileURLWithPath:value];
    __block BOOL opened = NO;
    const auto open_block = ^{
        opened = [NSWorkspace.sharedWorkspace openURL:url];
    };
    if (NSThread.isMainThread) {
        open_block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), open_block);
    }
    if (!opened) {
        error = "failed to open " + path.string();
        return false;
    }
    return true;
}

} // namespace

bool MacHostPlatform::open_config_file(const AppPaths& paths, std::string& error) {
    return ensure_config_file(paths, error) && open_path(paths.config_file, error);
}

bool MacHostPlatform::open_logs_directory(const AppPaths& paths, std::string& error) {
    return ensure_app_directories(paths, error) && open_path(paths.logs_directory, error);
}

bool MacHostPlatform::startup_registered(
    const std::filesystem::path& executable,
    bool& enabled,
    std::string& error) {
    (void)executable;
    bool requires_approval = false;
    return startup_registration_.status(enabled, requires_approval, error);
}

bool MacHostPlatform::set_startup_registered(
    const std::filesystem::path& executable,
    bool enabled,
    std::string& error) {
    (void)executable;
    return startup_registration_.set_registered(enabled, error);
}

bool MacHostPlatform::startup_status(
    bool& enabled,
    bool& requires_approval,
    std::string& error) {
    return startup_registration_.status(enabled, requires_approval, error);
}

} // namespace ccs
