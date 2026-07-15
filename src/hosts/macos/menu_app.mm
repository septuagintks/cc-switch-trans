#include "hosts/macos/menu_app.hpp"

#include "app/application_controller.hpp"
#include "app/control_executor.hpp"
#include "config/config_editing_service.hpp"
#include "config/config_store.hpp"
#include "hosts/macos/instance_coordinator.hpp"
#include "hosts/macos/main_window.hpp"
#include "hosts/macos/macos_host_platform.hpp"
#include "logging/logger.hpp"
#include "presentation/main_window_view_model.hpp"
#include "presentation/ui_preferences_store.hpp"

#import <AppKit/AppKit.h>

#include <filesystem>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

enum class MenuCommand {
    Status,
    Start,
    Stop,
    Reload,
    OpenConfig,
    OpenLogs,
    SetStartup,
};

struct MenuCommandResult {
    MenuCommand command = MenuCommand::Status;
    bool succeeded = false;
    bool startup_known = false;
    bool startup_enabled = false;
    bool startup_requires_approval = false;
    std::string error;
    ccs::ApplicationStatus status;
};

NSString* ns_string(const std::string& value) {
    NSString* result = [[NSString alloc] initWithBytes:value.data()
        length:value.size()
        encoding:NSUTF8StringEncoding];
    return result == nil ? @"" : result;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        reinterpret_cast<const char*>(value.data() + value.size()));
}

const char* command_name(MenuCommand command) {
    switch (command) {
    case MenuCommand::Status:
        return "status";
    case MenuCommand::Start:
        return "start";
    case MenuCommand::Stop:
        return "stop";
    case MenuCommand::Reload:
        return "reload";
    case MenuCommand::OpenConfig:
        return "open_config";
    case MenuCommand::OpenLogs:
        return "open_logs";
    case MenuCommand::SetStartup:
        return "set_startup";
    }
    return "unknown";
}

NSString* status_text(const ccs::ApplicationStatus& status) {
    std::string text = "Status: ";
    text += ccs::application_state_name(status.state);
    if (!status.listener_host.empty() && status.listener_port != 0) {
        text += " (" + status.listener_host + ":" + std::to_string(status.listener_port) + ")";
    }
    if (!status.last_error.empty()) {
        text += " - " + status.last_error;
    }
    return ns_string(text);
}

} // namespace

@interface CCSMenuDelegate : NSObject <NSApplicationDelegate, NSMenuDelegate> {
    ccs::AppPaths* _paths;
    std::filesystem::path* _executablePath;
    ccs::ApplicationController* _controller;
    ccs::MacHostPlatform* _platform;
    ccs::ControlExecutor* _executor;
    ccs::ConfigStore* _configRepository;
    ccs::ConfigEditingService* _configEditing;
    ccs::UiPreferencesStore* _uiPreferences;
    ccs::MainWindowViewModel* _viewModel;
    ccs::MacMainWindow* _mainWindow;
    ccs::MainWindowStateSnapshot _viewState;
    ccs::Logger* _logger;
    ccs::ApplicationStatus _cachedStatus;

    NSStatusItem* _statusItem;
    NSMenu* _menu;
    NSMenuItem* _statusMenuItem;
    NSMenuItem* _startMenuItem;
    NSMenuItem* _stopMenuItem;
    NSMenuItem* _reloadMenuItem;
    NSMenuItem* _lightweightMenuItem;
    NSMenuItem* _startupMenuItem;
    NSTimer* _statusTimer;
    dispatch_source_t _sigintSource;
    dispatch_source_t _sigtermSource;

    BOOL _statusPending;
    BOOL _serviceCommandPending;
    BOOL _startupKnown;
    BOOL _startupEnabled;
    BOOL _startupRequiresApproval;
    BOOL _automationEnabled;
    BOOL _exiting;
    BOOL _shutdownComplete;
    BOOL _terminateAfterShutdown;
    NSInteger _exitCode;
    std::uint64_t _lastViewCommandSequence;
    std::string _lastStatusError;
}

- (instancetype)initWithPaths:(ccs::AppPaths)paths
    executablePath:(std::filesystem::path)executablePath;
- (NSInteger)exitCode;
- (void)handleViewState:(ccs::MainWindowStateSnapshot)state;
- (void)beginShutdown:(NSString*)reason;
- (void)beginTerminationShutdown;
- (void)logHost:(const std::string&)level
    event:(const std::string&)event
    fields:(const std::vector<ccs::LogField>&)fields;

@end

@implementation CCSMenuDelegate

- (instancetype)initWithPaths:(ccs::AppPaths)paths
    executablePath:(std::filesystem::path)executablePath {
    self = [super init];
    if (self != nil) {
        _paths = new ccs::AppPaths(std::move(paths));
        _executablePath = new std::filesystem::path(std::move(executablePath));
        _controller = new ccs::ApplicationController(*_paths);
        _platform = new ccs::MacHostPlatform();
        _executor = new ccs::ControlExecutor();
        _configRepository = new ccs::ConfigStore(*_paths);
        _configEditing = new ccs::ConfigEditingService(*_configRepository);
        _uiPreferences = new ccs::UiPreferencesStore(*_paths);
        _viewModel = new ccs::MainWindowViewModel(
            *_configRepository,
            *_configEditing,
            *_controller,
            *_uiPreferences,
            [](std::function<void()> callback) {
                auto* posted = new std::function<void()>(std::move(callback));
                dispatch_async(dispatch_get_main_queue(), ^{
                    std::unique_ptr<std::function<void()>> owned(posted);
                    (*owned)();
                });
            },
            _executor);
    }
    return self;
}

- (void)dealloc {
    [_statusTimer invalidate];
    [NSDistributedNotificationCenter.defaultCenter removeObserver:self];
    if (_statusItem != nil) {
        [NSStatusBar.systemStatusBar removeStatusItem:_statusItem];
    }
    if (_viewModel != nullptr) {
        _viewModel->set_update_handler({});
    }
    if (_mainWindow != nullptr) {
        _mainWindow->destroy();
    }
    if (_viewModel != nullptr) {
        _viewModel->stop();
    }
    if (_executor != nullptr) {
        _executor->stop();
    }
    if (!_shutdownComplete && _controller != nullptr) {
        std::string error;
        (void)_controller->shutdown(error);
    }
    if (_logger != nullptr) {
        std::string error;
        (void)_logger->drain(error);
    }
    delete _logger;
    delete _mainWindow;
    delete _viewModel;
    delete _uiPreferences;
    delete _configEditing;
    delete _configRepository;
    delete _executor;
    delete _platform;
    delete _controller;
    delete _executablePath;
    delete _paths;
}

- (NSInteger)exitCode {
    return _exitCode;
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    std::string error;
    if (![self initializeHost:error]) {
        [self showError:@"ccs-trans startup failed" detail:ns_string(error)];
        _exitCode = 1;
        _shutdownComplete = YES;
        [NSApp terminate:nil];
    }
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    (void)sender;
    if (_shutdownComplete) {
        return NSTerminateNow;
    }
    if (_exiting) {
        return NSTerminateCancel;
    }
    __weak CCSMenuDelegate* weak_self = self;
    if (_mainWindow != nullptr
        && !_mainWindow->prepare_for_application_exit([weak_self]() {
            CCSMenuDelegate* delegate = weak_self;
            if (delegate != nil) {
                [delegate beginTerminationShutdown];
            }
        })) {
        return NSTerminateCancel;
    }
    [self beginTerminationShutdown];
    return NSTerminateCancel;
}

- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    [_statusTimer invalidate];
    if (_sigintSource != nil) {
        dispatch_source_cancel(_sigintSource);
    }
    if (_sigtermSource != nil) {
        dispatch_source_cancel(_sigtermSource);
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return NO;
}

- (BOOL)initializeHost:(std::string&)error {
    if (!ccs::ensure_app_directories(*_paths, error)) {
        return NO;
    }

    ccs::LoggerConfig logger_config;
    logger_config.path = _paths->host_log_file;
    logger_config.level = "info";
    logger_config.queue_capacity = 1024 * 1024;
    logger_config.max_total_size = 64ULL * 1024 * 1024;
    logger_config.flush_interval_ms = 100;
    _logger = new ccs::Logger(
        std::move(logger_config),
        nullptr,
        nullptr,
        [](const std::string& failure) {
            NSLog(@"ccs-trans host logger failed: %@", ns_string(failure));
        });
    std::string logger_error;
    if (!_logger->open(logger_error)) {
        delete _logger;
        _logger = nullptr;
        NSLog(@"ccs-trans host logging unavailable: %@", ns_string(logger_error));
    }

    NSString* icon_path = [NSBundle.mainBundle
        pathForResource:@"ccs-trans-statusTemplate"
        ofType:@"png"];
    NSImage* image = icon_path == nil ? nil : [[NSImage alloc] initWithContentsOfFile:icon_path];
    if (image == nil) {
        error = "failed to load the menu bar template image";
        return NO;
    }
    [image setTemplate:YES];
    image.size = NSMakeSize(18.0, 18.0);
    image.accessibilityDescription = @"ccs-trans";

    _statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
    if (_statusItem.button == nil) {
        error = "failed to create the menu bar status item";
        return NO;
    }
    _statusItem.button.image = image;
    _statusItem.button.toolTip = @"ccs-trans";

    _menu = [[NSMenu alloc] initWithTitle:@"ccs-trans"];
    _menu.delegate = self;
    [self addMenuItem:@"Open ccs-trans" action:@selector(openMainWindow:)];
    [_menu addItem:NSMenuItem.separatorItem];
    _statusMenuItem = [[NSMenuItem alloc] initWithTitle:@"Status: stopped"
        action:nil
        keyEquivalent:@""];
    _statusMenuItem.enabled = NO;
    [_menu addItem:_statusMenuItem];
    [_menu addItem:NSMenuItem.separatorItem];

    _startMenuItem = [self addMenuItem:@"Start" action:@selector(start:)];
    _stopMenuItem = [self addMenuItem:@"Stop" action:@selector(stop:)];
    _reloadMenuItem = [self addMenuItem:@"Reload Configuration" action:@selector(reload:)];
    [_menu addItem:NSMenuItem.separatorItem];
    [self addMenuItem:@"Open Configuration" action:@selector(openConfiguration:)];
    [self addMenuItem:@"Open Logs" action:@selector(openLogs:)];
    _lightweightMenuItem = [self addMenuItem:@"Lightweight Mode"
        action:@selector(toggleLightweight:)];
    _startupMenuItem = [self addMenuItem:@"Launch at Login" action:@selector(toggleStartup:)];
    [_menu addItem:NSMenuItem.separatorItem];
    [self addMenuItem:@"Quit" action:@selector(quit:)];
    _statusItem.menu = _menu;

    NSString* notification_name = [NSString stringWithUTF8String:ccs::kMacShowMenuNotification];
    NSString* notification_object = ns_string(path_to_utf8(
        _paths->state_directory / "menu-host.lock"));
    [NSDistributedNotificationCenter.defaultCenter
        addObserver:self
        selector:@selector(showExistingInstance:)
        name:notification_name
        object:notification_object
        suspensionBehavior:NSNotificationSuspensionBehaviorDeliverImmediately];

    _statusTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
        target:self
        selector:@selector(refreshStatus:)
        userInfo:nil
        repeats:YES];
    const char* automation = std::getenv("CCS_TRANS_MENU_TEST_AUTOMATION");
    _automationEnabled = automation != nullptr && std::string_view(automation) == "1";
    __weak CCSMenuDelegate* weak_self = self;
    _mainWindow = new ccs::MacMainWindow(
        *_viewModel,
        [weak_self](std::string_view event) {
            CCSMenuDelegate* delegate = weak_self;
            if (delegate != nil) {
                [delegate logHost:"info" event:"main_window_lifecycle" fields:{
                    ccs::field_string("action", std::string(event)),
                }];
            }
        });
    _viewModel->set_update_handler([weak_self](ccs::MainWindowStateSnapshot state) {
        CCSMenuDelegate* delegate = weak_self;
        if (delegate != nil) {
            [delegate handleViewState:std::move(state)];
        }
    });
    if (!_viewModel->submit({ccs::MainWindowCommand::LoadDraft})) {
        [self logHost:"error" event:"main_window_draft_load_rejected" fields:{}];
    }
    _sigintSource = [self terminationSignalSource:SIGINT];
    _sigtermSource = [self terminationSignalSource:SIGTERM];
    [self logHost:"info" event:"host_start" fields:{
        ccs::field_string("executable", path_to_utf8(*_executablePath)),
        ccs::field_string("config_root", path_to_utf8(_paths->root)),
    }];
    [self updateMenu];
    [self postCommand:MenuCommand::Status startupEnabled:NO];
    [self postCommand:MenuCommand::Start startupEnabled:NO];
    return YES;
}

- (dispatch_source_t)terminationSignalSource:(int)signalNumber {
    signal(signalNumber, SIG_IGN);
    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_SIGNAL,
        static_cast<uintptr_t>(signalNumber),
        0,
        dispatch_get_main_queue());
    __weak CCSMenuDelegate* weak_self = self;
    dispatch_source_set_event_handler(source, ^{
        CCSMenuDelegate* strong_self = weak_self;
        if (strong_self != nil && !strong_self->_exiting) {
            [strong_self beginShutdown:@"process_signal"];
        }
    });
    dispatch_resume(source);
    return source;
}

- (NSMenuItem*)addMenuItem:(NSString*)title action:(SEL)action {
    NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
    item.target = self;
    [_menu addItem:item];
    return item;
}

- (void)menuWillOpen:(NSMenu*)menu {
    (void)menu;
    [self updateMenu];
    [self postCommand:MenuCommand::Status startupEnabled:NO];
}

- (void)updateMenu {
    _statusMenuItem.title = status_text(_cachedStatus);
    const auto state = _cachedStatus.state;
    const bool transition = state == ccs::ApplicationState::Starting
        || state == ccs::ApplicationState::Reloading
        || state == ccs::ApplicationState::Stopping;
    _startMenuItem.enabled = !_exiting && !_serviceCommandPending && !transition
        && (state == ccs::ApplicationState::Stopped || state == ccs::ApplicationState::Faulted);
    _stopMenuItem.enabled = !_exiting && !_serviceCommandPending && !transition
        && state == ccs::ApplicationState::Running;
    _reloadMenuItem.enabled = _stopMenuItem.enabled;
    _lightweightMenuItem.enabled = !_exiting
        && _viewState
        && !_viewState->command_pending;
    _lightweightMenuItem.state = _viewState && _viewState->lightweight_mode
        ? NSControlStateValueOn
        : NSControlStateValueOff;
    _startupMenuItem.enabled = !_exiting && _startupKnown;
    _startupMenuItem.state = _startupEnabled ? NSControlStateValueOn : NSControlStateValueOff;
    _startupMenuItem.title = _startupRequiresApproval
        ? @"Launch at Login (Approval Required)"
        : @"Launch at Login";
}

- (void)openMainWindow:(id)sender {
    (void)sender;
    if (_exiting || _mainWindow == nullptr) {
        return;
    }
    std::string error;
    const auto state = _viewState ? _viewState : _viewModel->snapshot();
    if (!_mainWindow->show(state, error)) {
        [self logHost:"error" event:"main_window_show_failed" fields:{
            ccs::field_string("error", error),
        }];
        [self showError:@"ccs-trans window failed" detail:ns_string(error)];
    }
}

- (void)toggleLightweight:(id)sender {
    (void)sender;
    if (_exiting || _viewModel == nullptr || !_viewState) {
        return;
    }
    if (!_viewModel->submit({
            ccs::MainWindowCommand::SetLightweightMode,
            {},
            {},
            !_viewState->lightweight_mode})) {
        [self showError:@"ccs-trans command busy"
            detail:@"Wait for the current command to finish."];
    }
}

- (void)start:(id)sender {
    (void)sender;
    [self postCommand:MenuCommand::Start startupEnabled:NO];
}

- (void)stop:(id)sender {
    (void)sender;
    [self postCommand:MenuCommand::Stop startupEnabled:NO];
}

- (void)reload:(id)sender {
    (void)sender;
    [self postCommand:MenuCommand::Reload startupEnabled:NO];
}

- (void)openConfiguration:(id)sender {
    (void)sender;
    [self postCommand:MenuCommand::OpenConfig startupEnabled:NO];
}

- (void)openLogs:(id)sender {
    (void)sender;
    [self postCommand:MenuCommand::OpenLogs startupEnabled:NO];
}

- (void)toggleStartup:(id)sender {
    (void)sender;
    [self postCommand:MenuCommand::SetStartup startupEnabled:!_startupEnabled];
}

- (void)quit:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}

- (void)refreshStatus:(NSTimer*)timer {
    (void)timer;
    [self postCommand:MenuCommand::Status startupEnabled:NO];
}

- (void)showExistingInstance:(NSNotification*)notification {
    [self logHost:"info" event:"second_instance_notified" fields:{}];
    if (_exiting) {
        return;
    }
    NSString* test_command = _automationEnabled
        ? notification.userInfo[@"test_command"]
        : nil;
    if (test_command != nil && test_command.UTF8String != nullptr) {
        std::string error;
        const std::string command = test_command.UTF8String;
        bool succeeded = false;
        if (command == "show") {
            [self openMainWindow:nil];
            succeeded = true;
        } else if (command == "quit") {
            [NSApp terminate:nil];
            succeeded = true;
        } else if (command.starts_with("create-and-quit:")) {
            succeeded = _executor->post([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }) && _viewModel->submit({
                    ccs::MainWindowCommand::CreateProfile,
                    command.substr(std::string_view("create-and-quit:").size()),
                });
            dispatch_async(dispatch_get_main_queue(), ^{
                [NSApp terminate:nil];
            });
        } else if (_mainWindow != nullptr) {
            succeeded = _mainWindow->run_test_command(command, error);
        }
        [self logHost:(succeeded ? "info" : "error")
            event:"main_window_test_command"
            fields:{
                ccs::field_string("command", command),
                ccs::field_bool("succeeded", succeeded),
                ccs::field_string("error", error),
                ccs::field_bool("window_exists",
                    _mainWindow != nullptr && _mainWindow->exists()),
                ccs::field_bool("window_visible",
                    _mainWindow != nullptr && _mainWindow->visible()),
                ccs::field_number("application_window_count",
                    static_cast<long long>(NSApp.windows.count)),
                ccs::field_number("visible_application_window_count",
                    static_cast<long long>(
                        [NSWindow windowNumbersWithOptions:0].count)),
                ccs::field_number("live_main_window_count",
                    ccs::MacMainWindow::live_window_count()),
                ccs::field_number("live_main_window_controller_count",
                    ccs::MacMainWindow::live_controller_count()),
            }];
        return;
    }
    [self openMainWindow:nil];
}

- (void)postCommand:(MenuCommand)command startupEnabled:(BOOL)startupEnabled {
    if (_exiting) {
        return;
    }
    if (command == MenuCommand::Status) {
        if (_statusPending) {
            return;
        }
        _statusPending = YES;
    }
    const bool service_command = command == MenuCommand::Start
        || command == MenuCommand::Stop
        || command == MenuCommand::Reload;
    if (service_command) {
        if (_serviceCommandPending) {
            return;
        }
        _serviceCommandPending = YES;
    }
    if (command != MenuCommand::Status) {
        [self logHost:"info" event:"host_command_start" fields:{
            ccs::field_string("command", command_name(command)),
        }];
    }

    CCSMenuDelegate* delegate = self;
    const bool posted = _executor->post([delegate, command, startupEnabled]() {
        @autoreleasepool {
            auto* result = new MenuCommandResult();
            result->command = command;
            try {
                switch (command) {
                case MenuCommand::Status:
                    result->succeeded = true;
                    break;
                case MenuCommand::Start:
                    result->succeeded = delegate->_controller->start(result->error);
                    break;
                case MenuCommand::Stop:
                    result->succeeded = delegate->_controller->stop(result->error);
                    break;
                case MenuCommand::Reload:
                    result->succeeded = delegate->_controller->reload(result->error);
                    break;
                case MenuCommand::OpenConfig:
                    result->succeeded = delegate->_platform->open_config_file(
                        *delegate->_paths, result->error);
                    break;
                case MenuCommand::OpenLogs:
                    result->succeeded = delegate->_platform->open_logs_directory(
                        *delegate->_paths, result->error);
                    break;
                case MenuCommand::SetStartup:
                    result->succeeded = delegate->_platform->set_startup_registered(
                        *delegate->_executablePath, startupEnabled, result->error);
                    break;
                }
                result->status = delegate->_controller->status();
                std::string startup_error;
                result->startup_known = delegate->_platform->startup_status(
                    result->startup_enabled,
                    result->startup_requires_approval,
                    startup_error);
                if (!result->startup_known
                    && (command == MenuCommand::Status || command == MenuCommand::SetStartup)) {
                    if (result->error.empty()) {
                        result->error = std::move(startup_error);
                    }
                    result->succeeded = false;
                }
            } catch (const std::exception& ex) {
                result->succeeded = false;
                result->error = "host command failed: " + std::string(ex.what());
                result->status = delegate->_controller->status();
            } catch (...) {
                result->succeeded = false;
                result->error = "host command failed with an unknown exception";
                result->status = delegate->_controller->status();
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate handleCommandResult:result];
                delete result;
            });
        }
    });
    if (!posted) {
        if (command == MenuCommand::Status) {
            _statusPending = NO;
        }
        if (service_command) {
            _serviceCommandPending = NO;
        }
        [self logHost:"error" event:"host_command_rejected" fields:{
            ccs::field_string("command", command_name(command)),
        }];
    }
    [self updateMenu];
}

- (void)handleCommandResult:(MenuCommandResult*)result {
    if (result->command == MenuCommand::Status) {
        _statusPending = NO;
    }
    if (result->command == MenuCommand::Start
        || result->command == MenuCommand::Stop
        || result->command == MenuCommand::Reload) {
        _serviceCommandPending = NO;
    }
    const auto previous_state = _cachedStatus.state;
    _cachedStatus = result->status;
    if (_viewModel != nullptr) {
        _viewModel->refresh_application_status();
    }
    _startupKnown = result->startup_known;
    _startupEnabled = result->startup_enabled;
    _startupRequiresApproval = result->startup_requires_approval;

    if (result->command != MenuCommand::Status) {
        [self logHost:(result->succeeded ? "info" : "error")
            event:"host_command_complete"
            fields:{
                ccs::field_string("command", command_name(result->command)),
                ccs::field_bool("succeeded", result->succeeded),
                ccs::field_string("state", ccs::application_state_name(_cachedStatus.state)),
                ccs::field_string("error", result->error),
                ccs::field_bool("startup_requires_approval", result->startup_requires_approval),
            }];
    } else if (!result->succeeded && result->error != _lastStatusError) {
        [self logHost:"error" event:"host_status_failed" fields:{
            ccs::field_string("error", result->error),
        }];
        _lastStatusError = result->error;
    } else if (result->succeeded) {
        _lastStatusError.clear();
    }
    if (previous_state != _cachedStatus.state) {
        [self logHost:"info" event:"host_state_changed" fields:{
            ccs::field_string("previous_state", ccs::application_state_name(previous_state)),
            ccs::field_string("state", ccs::application_state_name(_cachedStatus.state)),
            ccs::field_number("exit_code", _cachedStatus.last_exit_code),
            ccs::field_string("error", _cachedStatus.last_error),
        }];
    }
    [self updateMenu];

    if (!result->succeeded && result->command != MenuCommand::Status) {
        NSString* detail = result->error.empty()
            ? @"The command failed. Open Logs for details."
            : ns_string(result->error);
        [self showError:@"ccs-trans command failed" detail:detail];
    } else if (previous_state != ccs::ApplicationState::Faulted
        && _cachedStatus.state == ccs::ApplicationState::Faulted) {
        [self showError:@"ccs-trans service stopped"
            detail:(_cachedStatus.last_error.empty()
                ? @"Open Logs for details."
                : ns_string(_cachedStatus.last_error))];
    }
}

- (void)handleViewState:(ccs::MainWindowStateSnapshot)state {
    if (!state) {
        return;
    }
    const bool previous_lightweight = _viewState && _viewState->lightweight_mode;
    _viewState = std::move(state);
    _cachedStatus = _viewState->application;
    if (_mainWindow != nullptr) {
        _mainWindow->update(_viewState);
    }

    if (_viewState->last_command
        && !_viewState->command_pending
        && _viewState->last_command->sequence > _lastViewCommandSequence) {
        const auto& result = *_viewState->last_command;
        _lastViewCommandSequence = result.sequence;
        [self logHost:(result.succeeded() || result.configuration_saved() ? "info" : "error")
            event:"main_window_command_complete"
            fields:{
                ccs::field_string("command", ccs::main_window_command_name(result.command)),
                ccs::field_string("outcome", ccs::command_outcome_name(result.outcome)),
                ccs::field_string("error", ccs::main_window_error_name(result.error)),
                ccs::field_string("detail", result.detail),
            }];
        if (!_automationEnabled
            && !result.succeeded()
            && !result.configuration_saved()
            && result.error != ccs::MainWindowError::Cancelled) {
            [self showError:@"ccs-trans command failed"
                detail:(result.detail.empty() ? @"Open Logs for details." : ns_string(result.detail))];
        }
    }

    if (!previous_lightweight
        && _viewState->lightweight_mode
        && _mainWindow != nullptr
        && ccs::resolve_cached_main_window(
                _viewState->draft,
                _mainWindow->exists(),
                _mainWindow->visible(),
                true)
            == ccs::CachedWindowAction::Destroy) {
        _mainWindow->destroy();
    }
    [self updateMenu];
}

- (void)beginShutdown:(NSString*)reason {
    if (_exiting) {
        return;
    }
    _exiting = YES;
    [_statusTimer invalidate];
    if (_sigintSource != nil) {
        dispatch_source_cancel(_sigintSource);
    }
    if (_sigtermSource != nil) {
        dispatch_source_cancel(_sigtermSource);
    }
    [self updateMenu];
    [self logHost:"info" event:"host_shutdown_start" fields:{
        ccs::field_string("reason", reason.UTF8String == nullptr ? "unknown" : reason.UTF8String),
    }];
    if (_viewModel != nullptr) {
        _viewModel->set_update_handler({});
    }
    if (_mainWindow != nullptr) {
        _mainWindow->destroy();
    }
    if (_viewModel != nullptr) {
        _viewModel->stop();
    }

    CCSMenuDelegate* delegate = self;
    if (!_executor->post([delegate]() {
            std::string error;
            const bool succeeded = delegate->_controller->shutdown(error);
            auto* result = new std::pair<bool, std::string>(succeeded, std::move(error));
            dispatch_async(dispatch_get_main_queue(), ^{
                [delegate finishShutdown:result->first error:result->second];
                delete result;
            });
        })) {
        [self finishShutdown:NO error:"control executor rejected shutdown"];
    }
}

- (void)beginTerminationShutdown {
    _terminateAfterShutdown = YES;
    [self beginShutdown:@"application_terminate"];
}

- (void)finishShutdown:(BOOL)succeeded error:(const std::string&)error {
    _shutdownComplete = YES;
    _exitCode = succeeded ? 0 : 1;
    [self logHost:(succeeded ? "info" : "error") event:"host_shutdown_complete" fields:{
        ccs::field_bool("succeeded", succeeded),
        ccs::field_string("error", error),
    }];
    _executor->stop();
    if (_logger != nullptr) {
        std::string drain_error;
        if (!_logger->drain(drain_error)) {
            NSLog(@"ccs-trans host logger drain failed: %@", ns_string(drain_error));
        }
    }
    if (_terminateAfterShutdown) {
        [NSApp terminate:nil];
    } else {
        [NSApp stop:nil];
        NSEvent* wake_event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
            location:NSZeroPoint
            modifierFlags:0
            timestamp:0
            windowNumber:0
            context:nil
            subtype:0
            data1:0
            data2:0];
        [NSApp postEvent:wake_event atStart:NO];
    }
}

- (void)showError:(NSString*)title detail:(NSString*)detail {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = title;
    alert.informativeText = detail;
    [alert runModal];
}

- (void)logHost:(const std::string&)level
    event:(const std::string&)event
    fields:(const std::vector<ccs::LogField>&)fields {
    if (_logger == nullptr || !_logger->log(level, event, fields)) {
        NSLog(@"ccs-trans %@ %@", ns_string(level), ns_string(event));
    }
}

@end

namespace ccs {

int run_menu_application(AppPaths paths, std::filesystem::path executable_path) {
    CCSMenuDelegate* delegate = [[CCSMenuDelegate alloc]
        initWithPaths:std::move(paths)
        executablePath:std::move(executable_path)];
    NSApplication* application = NSApplication.sharedApplication;
    application.delegate = delegate;
    [application run];
    application.delegate = nil;
    return static_cast<int>(delegate.exitCode);
}

} // namespace ccs
