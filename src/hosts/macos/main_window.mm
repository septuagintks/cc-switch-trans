#include "hosts/macos/main_window.hpp"

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

std::int64_t live_window_count = 0;
std::int64_t live_controller_count = 0;

constexpr CGFloat kMinimumWindowWidth = 800.0;
constexpr CGFloat kMinimumWindowHeight = 520.0;
constexpr CGFloat kNavigationEditorRatio = 16.0 / 84.0;
constexpr CGFloat kProfileListDetailsRatio = 38.0 / 62.0;
constexpr CGFloat kProfileColumnRatio = 0.36;
constexpr CGFloat kEnabledColumnRatio = 0.26;
constexpr CGFloat kReadinessColumnRatio = 0.38;
constexpr CGFloat kHeaderBrandRatio = 0.18;
constexpr CGFloat kHeaderStatusRatio = 0.34;
constexpr CGFloat kHeaderActionsRatio = 0.48;
constexpr CGFloat kNavigationButtonHeight = 50.0;
constexpr CGFloat kNavigationButtonSpacing = 6.0;
constexpr CGFloat kNavigationStackHeight =
    (3.0 * kNavigationButtonHeight) + (2.0 * kNavigationButtonSpacing);

bool approximately_equal(CGFloat value, CGFloat expected, CGFloat tolerance = 1.0) {
    return std::abs(value - expected) <= tolerance;
}

NSString* ns_string(const std::string& value) {
    NSString* result = [[NSString alloc] initWithBytes:value.data()
        length:value.size()
        encoding:NSUTF8StringEncoding];
    return result == nil ? @"" : result;
}

std::string utf8_string(NSString* value) {
    if (value == nil || value.UTF8String == nullptr) {
        return {};
    }
    return value.UTF8String;
}

NSString* readiness_text(ccs::ProfileReadiness readiness) {
    switch (readiness) {
    case ccs::ProfileReadiness::Incomplete:
        return @"Incomplete";
    case ccs::ProfileReadiness::Ready:
        return @"Ready";
    case ccs::ProfileReadiness::Invalid:
        return @"Invalid";
    }
    return @"Unknown";
}

NSColor* service_status_text_color(ccs::ApplicationState state) {
    switch (state) {
    case ccs::ApplicationState::Running:
        return NSColor.systemGreenColor;
    case ccs::ApplicationState::Faulted:
        return NSColor.systemRedColor;
    case ccs::ApplicationState::Starting:
    case ccs::ApplicationState::Reloading:
    case ccs::ApplicationState::Stopping:
        return NSColor.systemOrangeColor;
    case ccs::ApplicationState::Stopped:
    case ccs::ApplicationState::Shutdown:
        return NSColor.secondaryLabelColor;
    }
    return NSColor.secondaryLabelColor;
}

NSColor* service_status_background_color(ccs::ApplicationState state) {
    switch (state) {
    case ccs::ApplicationState::Running:
        return [NSColor.systemGreenColor colorWithAlphaComponent:0.16];
    case ccs::ApplicationState::Faulted:
        return [NSColor.systemRedColor colorWithAlphaComponent:0.16];
    case ccs::ApplicationState::Starting:
    case ccs::ApplicationState::Reloading:
    case ccs::ApplicationState::Stopping:
        return [NSColor.systemOrangeColor colorWithAlphaComponent:0.16];
    case ccs::ApplicationState::Stopped:
    case ccs::ApplicationState::Shutdown:
        return NSColor.secondarySystemFillColor;
    }
    return NSColor.secondarySystemFillColor;
}

NSTextField* label(NSString* value, NSString* accessibility_label) {
    NSTextField* field = [NSTextField labelWithString:value];
    field.accessibilityLabel = accessibility_label;
    field.lineBreakMode = NSLineBreakByTruncatingTail;
    return field;
}

NSButton* push_button(NSString* title, id target, SEL action, NSString* accessibility_label) {
    NSButton* button = [NSButton buttonWithTitle:title target:target action:action];
    button.bezelStyle = NSBezelStyleRounded;
    button.accessibilityLabel = accessibility_label;
    return button;
}

NSBox* rounded_editor_box(NSView* content) {
    NSBox* box = [[NSBox alloc] initWithFrame:NSZeroRect];
    box.boxType = NSBoxCustom;
    box.titlePosition = NSNoTitle;
    box.contentViewMargins = NSZeroSize;
    box.borderWidth = 1.0;
    box.cornerRadius = 8.0;
    box.borderColor = NSColor.separatorColor;
    box.fillColor = NSColor.textBackgroundColor;
    box.accessibilityElement = NO;
    box.wantsLayer = YES;
    [box.layer setCornerRadius:8.0];
    [box.layer setMasksToBounds:YES];

    NSView* host = box.contentView;
    if (host == nil) {
        host = [[NSView alloc] initWithFrame:NSZeroRect];
        box.contentView = host;
    }
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [host addSubview:content];
    [NSLayoutConstraint activateConstraints:@[
        [content.leadingAnchor constraintEqualToAnchor:host.leadingAnchor constant:1.0],
        [content.trailingAnchor constraintEqualToAnchor:host.trailingAnchor constant:-1.0],
        [content.topAnchor constraintEqualToAnchor:host.topAnchor constant:1.0],
        [content.bottomAnchor constraintEqualToAnchor:host.bottomAnchor constant:-1.0],
    ]];
    return box;
}

NSView* first_ambiguous_visible_view(NSView* view) {
    if (view.hidden) {
        return nil;
    }
    if (!view.translatesAutoresizingMaskIntoConstraints
        && view.hasAmbiguousLayout) {
        return view;
    }
    if ([view isKindOfClass:NSControl.class]
        || [view isKindOfClass:NSTextView.class]
        || [view isKindOfClass:NSTableView.class]) {
        return nil;
    }
    for (NSView* child in view.subviews) {
        NSView* ambiguous = first_ambiguous_visible_view(child);
        if (ambiguous != nil) {
            return ambiguous;
        }
    }
    return nil;
}

NSString* field_display_name(std::string_view key) {
    if (key == "field.listener.host") return @"Listener address";
    if (key == "field.listener.port") return @"Listener port";
    if (key == "field.runtime.worker_threads") return @"Worker threads";
    if (key == "field.runtime.max_connections") return @"Maximum connections";
    if (key == "field.runtime.max_request_body_size") return @"Request body limit (bytes)";
    if (key == "field.runtime.max_response_body_size") return @"Response body limit (bytes)";
    if (key == "field.runtime.max_inflight_bytes") return @"Inflight memory budget (bytes)";
    if (key == "field.runtime.metrics_interval_ms") return @"Metrics interval (ms)";
    if (key == "field.timeouts.resolve_ms") return @"Resolve timeout (ms)";
    if (key == "field.timeouts.connect_ms") return @"Connect timeout (ms)";
    if (key == "field.timeouts.send_ms") return @"Send timeout (ms)";
    if (key == "field.timeouts.response_header_ms") return @"Response header timeout (ms)";
    if (key == "field.timeouts.stream_idle_ms") return @"Stream idle timeout (ms)";
    if (key == "field.timeouts.total_ms") return @"Total timeout (ms)";
    if (key == "field.logging.path") return @"Log path";
    if (key == "field.logging.level") return @"Log level";
    if (key == "field.logging.body") return @"Record bodies";
    if (key == "field.logging.redact_sensitive") return @"Redact sensitive headers";
    if (key == "field.logging.body_limit") return @"Logged body limit (bytes)";
    if (key == "field.logging.queue_capacity") return @"Log queue capacity (bytes)";
    if (key == "field.logging.max_total_size") return @"Total log limit (bytes)";
    if (key == "field.logging.flush_interval_ms") return @"Log flush interval (ms)";
    if (key == "field.profile.protocol") return @"Protocol";
    if (key == "field.profile.local_request_path") return @"Local request path";
    if (key == "field.profile.local_usage_path") return @"Local usage path";
    if (key == "field.profile.upstream_base_url") return @"Upstream base URL";
    if (key == "field.profile.upstream_request_path") return @"Upstream request path";
    if (key == "field.profile.upstream_usage_path") return @"Upstream usage path";
    return @"Configuration field";
}

NSString* field_value_text(const ccs::ConfigurationFieldValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return ns_string(*text);
    }
    if (const auto* number = std::get_if<std::uint64_t>(&value)) {
        return [NSString stringWithFormat:@"%llu", static_cast<unsigned long long>(*number)];
    }
    return std::get<bool>(value) ? @"true" : @"false";
}

std::string canonical_newlines(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\r') {
            if (index + 1 < value.size() && value[index + 1] == '\n') {
                continue;
            }
            result.push_back('\n');
        } else {
            result.push_back(value[index]);
        }
    }
    return result;
}

const ccs::ConfigurationFieldState* find_field_state(
    const std::vector<ccs::ConfigurationFieldState>& fields,
    std::string_view key) {
    const auto found = std::find_if(fields.begin(), fields.end(), [key](const auto& field) {
        return field.key == key;
    });
    return found == fields.end() ? nullptr : &*found;
}

NSControl* configuration_field_control(
    const ccs::ConfigurationFieldDescriptor& descriptor,
    id target,
    SEL action) {
    const auto name = field_display_name(descriptor.display_name_key);
    if (descriptor.input_kind == ccs::ConfigurationFieldInputKind::Boolean) {
        NSButton* checkbox = [NSButton checkboxWithTitle:@"On" target:target action:action];
        checkbox.accessibilityLabel = name;
        return checkbox;
    }
    if (descriptor.input_kind == ccs::ConfigurationFieldInputKind::Enumeration) {
        NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
        popup.target = target;
        popup.action = action;
        popup.accessibilityLabel = name;
        if (!descriptor.required) {
            [popup addItemWithTitle:@"Not configured"];
        }
        for (const auto value : descriptor.enum_values) {
            [popup addItemWithTitle:ns_string(std::string(value))];
        }
        return popup;
    }
    NSTextField* field = [[NSTextField alloc] initWithFrame:NSZeroRect];
    field.accessibilityLabel = name;
    if (!descriptor.required) {
        field.placeholderString = @"Not configured";
    }
    return field;
}

} // namespace

typedef NS_ENUM(NSInteger, CCSMainWindowView) {
    CCSMainWindowViewProfiles,
    CCSMainWindowViewRules,
    CCSMainWindowViewSettings,
};

@interface CCSFlippedView : NSView
@end

@implementation CCSFlippedView
- (BOOL)isFlipped { return YES; }
@end

@interface CCSMainWindowResource : NSWindow
@end

@implementation CCSMainWindowResource

- (instancetype)initWithContentRect:(NSRect)contentRect
    styleMask:(NSWindowStyleMask)style
    backing:(NSBackingStoreType)bufferingType
    defer:(BOOL)flag {
    self = [super initWithContentRect:contentRect
        styleMask:style
        backing:bufferingType
        defer:flag];
    if (self != nil) {
        ++live_window_count;
    }
    return self;
}

- (void)dealloc {
    --live_window_count;
}

@end

@class CCSMainWindowController;

namespace ccs {

class MacMainWindow::Impl {
public:
    enum class CloseTarget {
        None,
        Hide,
        Destroy,
        ExitApplication,
    };

    struct PendingClose {
        CloseTarget target = CloseTarget::None;
        MainWindowCommand command = MainWindowCommand::Refresh;
        std::uint64_t previous_sequence = 0;
        std::function<void()> continuation;
    };

    Impl(MainWindowViewModel& view_model, LifecycleHandler lifecycle_handler);
    ~Impl();

    bool show(MainWindowStateSnapshot state, std::string& error);
    void update(MainWindowStateSnapshot state);
    bool prepare_for_application_exit(std::function<void()> continuation);
    void destroy();
    bool exists() const noexcept;
    bool visible() const noexcept;
    bool run_test_command(std::string_view command, std::string& error);

    MainWindowStateSnapshot state() const { return state_; }
    const ProfileListItem* selected_profile() const noexcept;
    bool submit(MainWindowCommandRequest request);
    void request_window_close(
        std::optional<UnsavedChangesDecision> decision = std::nullopt);

private:
    bool request_close(
        CloseTarget target,
        std::function<void()> continuation = {},
        std::optional<UnsavedChangesDecision> decision = std::nullopt);
    void finish_pending_close();
    void perform_close(CloseTarget target, std::function<void()> continuation = {});
    std::optional<UnsavedChangesDecision> prompt_unsaved_changes();
    void notify_lifecycle(std::string_view event) const;

    MainWindowViewModel& view_model_;
    LifecycleHandler lifecycle_handler_;
    MainWindowStateSnapshot state_;
    __strong CCSMainWindowController* controller_ = nil;
    PendingClose pending_close_;
};

} // namespace ccs

@interface CCSMainWindowController : NSWindowController <
    NSWindowDelegate,
    NSTableViewDataSource,
    NSTableViewDelegate,
    NSTextFieldDelegate,
    NSTextViewDelegate> {
    ccs::MacMainWindow::Impl* _owner;
    NSTextField* _brandLabel;
    NSBox* _serviceStatusBadge;
    NSTextField* _serviceStatus;
    NSTextField* _listenerStatus;
    NSButton* _startButton;
    NSButton* _stopButton;
    NSButton* _reloadButton;
    NSButton* _lightweightCheckbox;
    NSView* _serviceRow;
    NSView* _headerBrandRegion;
    NSView* _headerStatusRegion;
    NSView* _headerActionsRegion;
    NSStackView* _serviceActions;
    NSButton* _profilesNavigationButton;
    NSButton* _rulesNavigationButton;
    NSButton* _settingsNavigationButton;
    NSStackView* _navigation;
    NSView* _editorHost;
    NSStackView* _workspace;
    NSView* _profilesView;
    NSView* _rulesView;
    NSView* _settingsView;
    NSStackView* _profilesList;
    NSStackView* _profilesLayout;
    NSBox* _profileListFrame;
    NSScrollView* _profileTableScroll;
    NSScrollView* _profileDetailsScroll;
    CCSFlippedView* _profileDetailsDocument;
    NSStackView* _profileDetailsContent;
    NSArray<NSView*>* _profileDetailRows;
    NSArray<NSView*>* _profileFieldRows;
    NSTableView* _profileTable;
    NSTableColumn* _profileColumn;
    NSTableColumn* _enabledColumn;
    NSTableColumn* _readinessColumn;
    NSTextField* _newProfileField;
    NSButton* _addButton;
    NSButton* _removeButton;
    NSTextField* _renameField;
    NSButton* _renameButton;
    NSButton* _enabledCheckbox;
    NSTextField* _protocolValue;
    NSTextField* _readinessValue;
    NSTextField* _rulesValue;
    NSTextField* _profileDetail;
    NSMutableArray<NSDictionary<NSString*, id>*>* _profileFieldControls;
    NSButton* _updateProfileFieldsButton;
    NSPopUpButton* _rulesProfilePopup;
    NSTextView* _rulesTextView;
    NSTextField* _rulesStatus;
    NSButton* _formatRulesButton;
    NSButton* _updateRulesButton;
    NSBox* _rulesEditorFrame;
    NSMutableArray<NSDictionary<NSString*, id>*>* _settingsFieldControls;
    NSArray<NSView*>* _settingsFieldRows;
    NSButton* _updateSettingsButton;
    NSTextField* _commandStatus;
    NSButton* _reloadDraftButton;
    NSButton* _applyButton;
    NSButton* _discardButton;
    NSString* _localStatus;
    BOOL _localStatusIsError;
    CCSMainWindowView _currentView;
    BOOL _profileLocalDirty;
    BOOL _rulesLocalDirty;
    BOOL _settingsLocalDirty;
    BOOL _profileUpdateAwaiting;
    BOOL _rulesUpdateAwaiting;
    BOOL _settingsUpdateAwaiting;
    std::uint64_t _profileUpdatePreviousSequence;
    std::uint64_t _rulesUpdatePreviousSequence;
    std::uint64_t _settingsUpdatePreviousSequence;
    BOOL _hasProfileLocalKey;
    BOOL _hasRulesLocalKey;
    ccs::ProfileKey _profileLocalKey;
    ccs::ProfileKey _rulesLocalKey;
    BOOL _updating;
    BOOL _resourceCounted;
}

- (instancetype)initWithOwner:(ccs::MacMainWindow::Impl*)owner;
- (void)invalidateOwner;
- (void)render;
- (void)prepareForDisplay:(BOOL)resetScroll;
- (void)updateResponsiveLayout;
- (void)showView:(CCSMainWindowView)view;
- (CCSMainWindowView)currentView;
- (void)configureKeyLoop;
- (void)setLocalStatus:(NSString*)message error:(BOOL)error;
- (BOOL)hasLocalEdits;
- (void)discardLocalEdits;
- (BOOL)confirmDiscardLocalEdits;
- (BOOL)confirmDiscardProfileEditors;
- (void)resolvePendingEditorUpdates;
- (void)updateEnabledStates;
- (void)populateFieldControls:(NSArray<NSDictionary<NSString*, id>*>*)entries
    states:(const std::vector<ccs::ConfigurationFieldState>&)states;
- (BOOL)collectFieldEdits:(NSArray<NSDictionary<NSString*, id>*>*)entries
    states:(const std::vector<ccs::ConfigurationFieldState>&)states
    edits:(std::vector<ccs::ConfigurationFieldEdit>&)edits
    error:(std::string&)error;
- (BOOL)validateLayout:(std::string&)error;
- (BOOL)validateResponsiveLayout:(std::string&)error;
- (BOOL)validateResponsiveBehavior:(std::string&)error;
- (BOOL)validateKeyboard:(std::string&)error;
- (BOOL)validateRetina:(std::string&)error;
- (BOOL)validateProfileRuleSummary:(std::string&)error;

@end

@implementation CCSMainWindowController

- (instancetype)initWithOwner:(ccs::MacMainWindow::Impl*)owner {
    NSWindow* window = [[CCSMainWindowResource alloc]
        initWithContentRect:NSMakeRect(0.0, 0.0, 1120.0, 720.0)
        styleMask:(NSWindowStyleMaskTitled
            | NSWindowStyleMaskClosable
            | NSWindowStyleMaskMiniaturizable
            | NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered
        defer:NO];
    self = [super initWithWindow:window];
    if (self == nil) {
        return nil;
    }
    _resourceCounted = YES;
    ++live_controller_count;
    _owner = owner;
    window.title = @"ccs-trans";
    window.minSize = NSMakeSize(kMinimumWindowWidth, kMinimumWindowHeight);
    window.restorable = NO;
    window.tabbingMode = NSWindowTabbingModeDisallowed;
    window.excludedFromWindowsMenu = YES;
    window.autorecalculatesKeyViewLoop = NO;
    window.delegate = self;
    [window center];

    _brandLabel = label(@"ccs-trans", @"ccs-trans");
    _brandLabel.font = [NSFont systemFontOfSize:22.0 weight:NSFontWeightSemibold];
    _serviceStatus = label(@"Stopped", @"Service status");
    _serviceStatus.font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightSemibold];
    _serviceStatus.alignment = NSTextAlignmentCenter;
    _serviceStatus.textColor = service_status_text_color(ccs::ApplicationState::Stopped);
    _serviceStatus.usesSingleLineMode = YES;
    _serviceStatus.maximumNumberOfLines = 1;
    [_serviceStatus setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_serviceStatus setContentCompressionResistancePriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    _serviceStatusBadge = [[NSBox alloc] initWithFrame:NSZeroRect];
    _serviceStatusBadge.boxType = NSBoxCustom;
    _serviceStatusBadge.titlePosition = NSNoTitle;
    _serviceStatusBadge.contentViewMargins = NSZeroSize;
    _serviceStatusBadge.borderWidth = 0.0;
    _serviceStatusBadge.cornerRadius = 12.0;
    _serviceStatusBadge.fillColor =
        service_status_background_color(ccs::ApplicationState::Stopped);
    _serviceStatusBadge.accessibilityElement = NO;
    NSView* serviceStatusContent = _serviceStatusBadge.contentView;
    if (serviceStatusContent == nil) {
        serviceStatusContent = [[NSView alloc] initWithFrame:NSZeroRect];
        _serviceStatusBadge.contentView = serviceStatusContent;
    }
    _serviceStatus.translatesAutoresizingMaskIntoConstraints = NO;
    [serviceStatusContent addSubview:_serviceStatus];
    [NSLayoutConstraint activateConstraints:@[
        [_serviceStatus.centerXAnchor
            constraintEqualToAnchor:serviceStatusContent.centerXAnchor],
        [_serviceStatus.centerYAnchor
            constraintEqualToAnchor:serviceStatusContent.centerYAnchor],
        [_serviceStatus.leadingAnchor
            constraintGreaterThanOrEqualToAnchor:serviceStatusContent.leadingAnchor
            constant:10.0],
        [_serviceStatus.trailingAnchor
            constraintLessThanOrEqualToAnchor:serviceStatusContent.trailingAnchor
            constant:-10.0],
        [_serviceStatusBadge.widthAnchor constraintEqualToConstant:92.0],
        [_serviceStatusBadge.heightAnchor constraintEqualToConstant:24.0],
    ]];
    [_serviceStatusBadge setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_serviceStatusBadge setContentCompressionResistancePriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    _listenerStatus = label(@"Listener inactive", @"Listener address");
    _startButton = push_button(@"Start", self, @selector(startService:), @"Start service");
    _stopButton = push_button(@"Stop", self, @selector(stopService:), @"Stop service");
    _reloadButton = push_button(@"Reload", self, @selector(reloadService:), @"Reload service");
    _lightweightCheckbox = [NSButton checkboxWithTitle:@"Lightweight Mode"
        target:self
        action:@selector(toggleLightweight:)];
    _lightweightCheckbox.accessibilityLabel = @"Lightweight Mode";

    NSStackView* serviceText = [NSStackView stackViewWithViews:@[
        _serviceStatusBadge, _listenerStatus]];
    serviceText.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    serviceText.alignment = NSLayoutAttributeCenterY;
    serviceText.distribution = NSStackViewDistributionFill;
    serviceText.spacing = 14.0;
    [_listenerStatus setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    _serviceActions = [NSStackView stackViewWithViews:@[
        _startButton, _stopButton, _reloadButton, _lightweightCheckbox]];
    _serviceActions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _serviceActions.alignment = NSLayoutAttributeCenterY;
    _serviceActions.spacing = 8.0;
    [_serviceActions setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_serviceActions setContentCompressionResistancePriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    for (NSView* action in _serviceActions.arrangedSubviews) {
        [action setContentHuggingPriority:NSLayoutPriorityRequired
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [action setContentCompressionResistancePriority:NSLayoutPriorityRequired
            forOrientation:NSLayoutConstraintOrientationHorizontal];
    }

    _serviceRow = [[NSView alloc] initWithFrame:NSZeroRect];
    _serviceRow.identifier = @"Header layout";
    _headerBrandRegion = [[NSView alloc] initWithFrame:NSZeroRect];
    _headerStatusRegion = [[NSView alloc] initWithFrame:NSZeroRect];
    _headerActionsRegion = [[NSView alloc] initWithFrame:NSZeroRect];
    for (NSView* region in @[
             _headerBrandRegion, _headerStatusRegion, _headerActionsRegion]) {
        region.translatesAutoresizingMaskIntoConstraints = NO;
        [_serviceRow addSubview:region];
    }
    _brandLabel.translatesAutoresizingMaskIntoConstraints = NO;
    serviceText.translatesAutoresizingMaskIntoConstraints = NO;
    _serviceActions.translatesAutoresizingMaskIntoConstraints = NO;
    [_headerBrandRegion addSubview:_brandLabel];
    [_headerStatusRegion addSubview:serviceText];
    [_headerActionsRegion addSubview:_serviceActions];
    [NSLayoutConstraint activateConstraints:@[
        [_headerBrandRegion.leadingAnchor constraintEqualToAnchor:_serviceRow.leadingAnchor],
        [_headerStatusRegion.leadingAnchor constraintEqualToAnchor:_headerBrandRegion.trailingAnchor],
        [_headerActionsRegion.leadingAnchor constraintEqualToAnchor:_headerStatusRegion.trailingAnchor],
        [_headerActionsRegion.trailingAnchor constraintEqualToAnchor:_serviceRow.trailingAnchor],
        [_headerBrandRegion.topAnchor constraintEqualToAnchor:_serviceRow.topAnchor],
        [_headerBrandRegion.bottomAnchor constraintEqualToAnchor:_serviceRow.bottomAnchor],
        [_headerStatusRegion.topAnchor constraintEqualToAnchor:_serviceRow.topAnchor],
        [_headerStatusRegion.bottomAnchor constraintEqualToAnchor:_serviceRow.bottomAnchor],
        [_headerActionsRegion.topAnchor constraintEqualToAnchor:_serviceRow.topAnchor],
        [_headerActionsRegion.bottomAnchor constraintEqualToAnchor:_serviceRow.bottomAnchor],
        [_headerBrandRegion.widthAnchor constraintEqualToAnchor:_serviceRow.widthAnchor
            multiplier:kHeaderBrandRatio],
        [_headerStatusRegion.widthAnchor constraintEqualToAnchor:_serviceRow.widthAnchor
            multiplier:kHeaderStatusRatio],
        [_headerActionsRegion.widthAnchor constraintEqualToAnchor:_serviceRow.widthAnchor
            multiplier:kHeaderActionsRatio],
        [_brandLabel.leadingAnchor constraintEqualToAnchor:_headerBrandRegion.leadingAnchor],
        [_brandLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_headerBrandRegion.trailingAnchor
            constant:-8.0],
        [_brandLabel.centerYAnchor constraintEqualToAnchor:_headerBrandRegion.centerYAnchor],
        [serviceText.leadingAnchor constraintEqualToAnchor:_headerStatusRegion.leadingAnchor],
        [serviceText.trailingAnchor constraintLessThanOrEqualToAnchor:_headerStatusRegion.trailingAnchor
            constant:-8.0],
        [serviceText.centerYAnchor constraintEqualToAnchor:_headerStatusRegion.centerYAnchor],
        [_serviceActions.leadingAnchor constraintGreaterThanOrEqualToAnchor:_headerActionsRegion.leadingAnchor],
        [_serviceActions.trailingAnchor constraintEqualToAnchor:_headerActionsRegion.trailingAnchor],
        [_serviceActions.centerYAnchor constraintEqualToAnchor:_headerActionsRegion.centerYAnchor],
    ]];
    [_serviceRow setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_serviceRow setContentCompressionResistancePriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_serviceRow.heightAnchor constraintEqualToConstant:32.0].active = YES;

    _profilesNavigationButton = push_button(
        @"Profiles", self, @selector(showProfiles:), @"Show Profiles editor");
    _rulesNavigationButton = push_button(
        @"Rules", self, @selector(showRules:), @"Show Rules editor");
    _settingsNavigationButton = push_button(
        @"Settings", self, @selector(showSettings:), @"Show Settings editor");
    for (NSButton* button in @[
             _profilesNavigationButton, _rulesNavigationButton, _settingsNavigationButton]) {
        [button setButtonType:NSButtonTypeToggle];
        button.alignment = NSTextAlignmentCenter;
        button.bezelStyle = NSBezelStyleRegularSquare;
        button.wantsLayer = YES;
        button.layer.cornerRadius = 8.0;
        button.layer.masksToBounds = YES;
    }
    _navigation = [NSStackView stackViewWithViews:@[
        _profilesNavigationButton, _rulesNavigationButton, _settingsNavigationButton]];
    _navigation.identifier = @"Navigation layout";
    _navigation.orientation = NSUserInterfaceLayoutOrientationVertical;
    _navigation.alignment = NSLayoutAttributeLeading;
    _navigation.distribution = NSStackViewDistributionFill;
    _navigation.spacing = kNavigationButtonSpacing;
    for (NSButton* button in @[
             _profilesNavigationButton, _rulesNavigationButton, _settingsNavigationButton]) {
        [button.widthAnchor constraintEqualToAnchor:_navigation.widthAnchor].active = YES;
        [button.heightAnchor constraintEqualToConstant:kNavigationButtonHeight].active = YES;
    }
    [_navigation.heightAnchor constraintEqualToConstant:kNavigationStackHeight].active = YES;
    [_navigation setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_navigation setContentCompressionResistancePriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationVertical];

    _profileTable = [[NSTableView alloc] initWithFrame:NSZeroRect];
    _profileTable.dataSource = self;
    _profileTable.delegate = self;
    _profileTable.allowsMultipleSelection = NO;
    _profileTable.allowsEmptySelection = YES;
    _profileTable.usesAlternatingRowBackgroundColors = YES;
    _profileTable.columnAutoresizingStyle = NSTableViewNoColumnAutoresizing;
    _profileTable.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _profileTable.accessibilityLabel = @"Profiles";
    _profileColumn = [[NSTableColumn alloc] initWithIdentifier:@"profile"];
    _profileColumn.title = @"Profile";
    _profileColumn.width = 110.0;
    _profileColumn.minWidth = 24.0;
    _profileColumn.resizingMask = NSTableColumnNoResizing;
    _enabledColumn = [[NSTableColumn alloc] initWithIdentifier:@"enabled"];
    _enabledColumn.title = @"Enabled";
    _enabledColumn.width = 68.0;
    _enabledColumn.minWidth = 24.0;
    _enabledColumn.resizingMask = NSTableColumnNoResizing;
    _readinessColumn = [[NSTableColumn alloc] initWithIdentifier:@"readiness"];
    _readinessColumn.title = @"Readiness";
    _readinessColumn.width = 90.0;
    _readinessColumn.minWidth = 24.0;
    _readinessColumn.resizingMask = NSTableColumnNoResizing;
    [_profileTable addTableColumn:_profileColumn];
    [_profileTable addTableColumn:_enabledColumn];
    [_profileTable addTableColumn:_readinessColumn];
    _profileTableScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _profileTableScroll.documentView = _profileTable;
    _profileTableScroll.hasVerticalScroller = YES;
    _profileTableScroll.hasHorizontalScroller = NO;
    _profileTableScroll.autohidesScrollers = YES;
    _profileTableScroll.borderType = NSNoBorder;
    _profileListFrame = rounded_editor_box(_profileTableScroll);

    _newProfileField = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _newProfileField.placeholderString = @"New Profile ID";
    _newProfileField.accessibilityLabel = @"New Profile ID";
    NSImage* addImage = [NSImage imageWithSystemSymbolName:@"plus" accessibilityDescription:@"Add Profile"];
    NSImage* removeImage = [NSImage imageWithSystemSymbolName:@"trash" accessibilityDescription:@"Remove Profile"];
    _addButton = [NSButton buttonWithImage:addImage target:self action:@selector(addProfile:)];
    _addButton.accessibilityLabel = @"Add Profile";
    _addButton.toolTip = @"Add Profile";
    _removeButton = [NSButton buttonWithImage:removeImage target:self action:@selector(removeProfile:)];
    _removeButton.accessibilityLabel = @"Remove selected Profile";
    _removeButton.toolTip = @"Remove selected Profile";
    _addButton.bezelStyle = NSBezelStyleRounded;
    _removeButton.bezelStyle = NSBezelStyleRounded;
    [_addButton.widthAnchor constraintEqualToConstant:32.0].active = YES;
    [_removeButton.widthAnchor constraintEqualToConstant:32.0].active = YES;
    NSStackView* addRow = [NSStackView stackViewWithViews:@[
        _newProfileField, _addButton, _removeButton]];
    addRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    addRow.spacing = 8.0;
    [_newProfileField setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSTextField* profilesHeading = label(@"Profiles", @"Profiles list heading");
    profilesHeading.font = [NSFont systemFontOfSize:17.0 weight:NSFontWeightSemibold];
    _profilesList = [NSStackView stackViewWithViews:@[
        profilesHeading, _profileListFrame, addRow]];
    _profilesList.identifier = @"Profiles list layout";
    _profilesList.orientation = NSUserInterfaceLayoutOrientationVertical;
    _profilesList.alignment = NSLayoutAttributeLeading;
    _profilesList.distribution = NSStackViewDistributionFill;
    _profilesList.spacing = 8.0;
    [_profileListFrame.widthAnchor constraintEqualToAnchor:_profilesList.widthAnchor].active = YES;
    [addRow.widthAnchor constraintEqualToAnchor:_profilesList.widthAnchor].active = YES;
    [_profileListFrame setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_profileListFrame setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_profileListFrame.heightAnchor constraintGreaterThanOrEqualToConstant:80.0].active = YES;

    _renameField = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _renameField.placeholderString = @"Profile ID";
    _renameField.accessibilityLabel = @"Selected Profile ID";
    _renameButton = push_button(@"Rename", self, @selector(renameProfile:), @"Rename selected Profile");
    NSStackView* renameRow = [NSStackView stackViewWithViews:@[_renameField, _renameButton]];
    renameRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    renameRow.spacing = 8.0;
    _enabledCheckbox = [NSButton checkboxWithTitle:@"Enabled"
        target:self
        action:@selector(toggleProfileEnabled:)];
    _enabledCheckbox.accessibilityLabel = @"Profile enabled";
    _protocolValue = label(@"", @"Profile protocol");
    _readinessValue = label(@"No selection", @"Profile readiness");
    _rulesValue = label(@"0 enabled / 0 total", @"Profile Rule summary");
    _profileDetail = label(@"Select or create a Profile.", @"Profile validation detail");
    _profileDetail.lineBreakMode = NSLineBreakByWordWrapping;
    _profileDetail.maximumNumberOfLines = 0;
    [_profileDetail setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    _profileFieldControls = [NSMutableArray array];
    NSMutableArray<NSView*>* profileFieldRows = [NSMutableArray array];
    for (const auto& descriptor : ccs::profile_field_descriptors()) {
        if (descriptor.key == "id" || descriptor.key == "enabled") {
            continue;
        }
        const auto name = field_display_name(descriptor.display_name_key);
        NSControl* input = configuration_field_control(
            descriptor, self, @selector(profileFieldChanged:));
        input.identifier = ns_string(std::string(descriptor.key));
        if ([input isKindOfClass:NSTextField.class]) {
            static_cast<NSTextField*>(input).delegate = self;
        }
        [_profileFieldControls addObject:@{
            @"key": ns_string(std::string(descriptor.key)),
            @"input": input,
            @"kind": @(static_cast<NSInteger>(descriptor.input_kind)),
            @"required": @(descriptor.required),
            @"name": name,
        }];
        NSTextField* fieldLabel = label(name, name);
        NSStackView* row = [NSStackView stackViewWithViews:@[fieldLabel, input]];
        row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        row.alignment = NSLayoutAttributeCenterY;
        row.distribution = NSStackViewDistributionFill;
        row.spacing = 14.0;
        [fieldLabel.widthAnchor constraintEqualToAnchor:input.widthAnchor
            multiplier:(34.0 / 66.0)].active = YES;
        [fieldLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [input setContentHuggingPriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [input setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [profileFieldRows addObject:row];
    }
    _profileFieldRows = [profileFieldRows copy];
    _updateProfileFieldsButton = push_button(
        @"Update Profile", self, @selector(updateProfileFields:), @"Update Profile fields");
    NSTextField* profileDetailsHeading = label(@"Profile details", @"Profile details heading");
    profileDetailsHeading.font = [NSFont systemFontOfSize:17.0 weight:NSFontWeightSemibold];
    NSStackView* readinessRow = [NSStackView stackViewWithViews:@[
        label(@"Readiness", @"Readiness label"), _readinessValue,
        label(@"Rules", @"Rules label"), _rulesValue]];
    readinessRow.identifier = @"Profile summary row";
    readinessRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    readinessRow.alignment = NSLayoutAttributeCenterY;
    readinessRow.spacing = 8.0;
    [_readinessValue setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_readinessValue setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* profileUpdateRow = [NSStackView stackViewWithViews:@[
        _profileDetail, _updateProfileFieldsButton]];
    profileUpdateRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    profileUpdateRow.alignment = NSLayoutAttributeCenterY;
    profileUpdateRow.spacing = 12.0;
    [_profileDetail setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSMutableArray<NSView*>* orderedDetailRows = [NSMutableArray arrayWithObjects:
        profileDetailsHeading, readinessRow, renameRow, _enabledCheckbox, nil];
    [orderedDetailRows addObjectsFromArray:profileFieldRows];
    [orderedDetailRows addObject:profileUpdateRow];
    _profileDetailRows = [orderedDetailRows copy];
    _profileDetailsContent = [NSStackView stackViewWithViews:orderedDetailRows];
    _profileDetailsContent.identifier = @"Profile details layout";
    _profileDetailsContent.orientation = NSUserInterfaceLayoutOrientationVertical;
    _profileDetailsContent.alignment = NSLayoutAttributeLeading;
    _profileDetailsContent.distribution = NSStackViewDistributionFill;
    _profileDetailsContent.spacing = 10.0;
    [renameRow.widthAnchor constraintEqualToAnchor:_profileDetailsContent.widthAnchor].active = YES;
    [readinessRow.widthAnchor
        constraintEqualToAnchor:_profileDetailsContent.widthAnchor].active = YES;
    for (NSView* row in profileFieldRows) {
        [row.widthAnchor constraintEqualToAnchor:_profileDetailsContent.widthAnchor].active = YES;
    }
    [profileUpdateRow.widthAnchor
        constraintEqualToAnchor:_profileDetailsContent.widthAnchor].active = YES;

    _profileDetailsDocument = [[CCSFlippedView alloc]
        initWithFrame:NSZeroRect];
    _profileDetailsDocument.translatesAutoresizingMaskIntoConstraints = NO;
    _profileDetailsContent.translatesAutoresizingMaskIntoConstraints = NO;
    [_profileDetailsDocument addSubview:_profileDetailsContent];
    [NSLayoutConstraint activateConstraints:@[
        [_profileDetailsContent.leadingAnchor
            constraintEqualToAnchor:_profileDetailsDocument.leadingAnchor constant:12.0],
        [_profileDetailsContent.trailingAnchor
            constraintEqualToAnchor:_profileDetailsDocument.trailingAnchor constant:-12.0],
        [_profileDetailsContent.topAnchor
            constraintEqualToAnchor:_profileDetailsDocument.topAnchor constant:8.0],
        [_profileDetailsDocument.bottomAnchor
            constraintEqualToAnchor:_profileDetailsContent.bottomAnchor
            constant:8.0],
    ]];
    _profileDetailsScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _profileDetailsScroll.documentView = _profileDetailsDocument;
    _profileDetailsScroll.hasVerticalScroller = YES;
    _profileDetailsScroll.hasHorizontalScroller = NO;
    _profileDetailsScroll.autohidesScrollers = YES;
    _profileDetailsScroll.borderType = NSNoBorder;
    [_profileDetailsDocument.leadingAnchor
        constraintEqualToAnchor:_profileDetailsScroll.contentView.leadingAnchor].active = YES;
    [_profileDetailsDocument.topAnchor
        constraintEqualToAnchor:_profileDetailsScroll.contentView.topAnchor].active = YES;
    [_profileDetailsDocument.widthAnchor
        constraintEqualToAnchor:_profileDetailsScroll.contentView.widthAnchor].active = YES;

    _profilesLayout = [NSStackView stackViewWithViews:@[
        _profilesList, _profileDetailsScroll]];
    _profilesLayout.identifier = @"Profiles editor layout";
    _profilesLayout.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _profilesLayout.alignment = NSLayoutAttributeTop;
    _profilesLayout.distribution = NSStackViewDistributionFill;
    _profilesLayout.spacing = 16.0;
    [_profilesList.heightAnchor constraintEqualToAnchor:_profilesLayout.heightAnchor].active = YES;
    [_profileDetailsScroll.heightAnchor
        constraintEqualToAnchor:_profilesLayout.heightAnchor].active = YES;
    [_profilesList.widthAnchor constraintEqualToAnchor:_profileDetailsScroll.widthAnchor
        multiplier:kProfileListDetailsRatio].active = YES;
    [_profilesList setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_profileDetailsScroll setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    _profilesView = _profilesLayout;

    _rulesProfilePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    _rulesProfilePopup.target = self;
    _rulesProfilePopup.action = @selector(selectRulesProfile:);
    _rulesProfilePopup.accessibilityLabel = @"Rules Profile";
    _formatRulesButton = push_button(
        @"Format", self, @selector(formatRules:), @"Format Rules text");
    _updateRulesButton = push_button(
        @"Update Rules", self, @selector(updateRules:), @"Update Rules text");
    NSTextField* rulesHeading = label(@"Rules", @"Rules editor heading");
    rulesHeading.font = [NSFont systemFontOfSize:17.0 weight:NSFontWeightSemibold];
    NSStackView* rulesActions = [NSStackView stackViewWithViews:@[
        _rulesProfilePopup, _formatRulesButton, _updateRulesButton]];
    rulesActions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    rulesActions.alignment = NSLayoutAttributeCenterY;
    rulesActions.distribution = NSStackViewDistributionFill;
    rulesActions.spacing = 8.0;
    [_rulesProfilePopup setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1.0
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_rulesProfilePopup setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    for (NSButton* button in @[_formatRulesButton, _updateRulesButton]) {
        [button setContentHuggingPriority:NSLayoutPriorityRequired
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [button setContentCompressionResistancePriority:NSLayoutPriorityRequired
            forOrientation:NSLayoutConstraintOrientationHorizontal];
    }
    _rulesTextView = [[NSTextView alloc] initWithFrame:NSZeroRect];
    _rulesTextView.delegate = self;
    _rulesTextView.accessibilityLabel = @"Rules JSON";
    _rulesTextView.font = [NSFont monospacedSystemFontOfSize:12.0 weight:NSFontWeightRegular];
    _rulesTextView.automaticQuoteSubstitutionEnabled = NO;
    _rulesTextView.automaticDashSubstitutionEnabled = NO;
    _rulesTextView.automaticTextReplacementEnabled = NO;
    _rulesTextView.textContainerInset = NSMakeSize(8.0, 8.0);
    _rulesTextView.minSize = NSMakeSize(0.0, 0.0);
    _rulesTextView.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    _rulesTextView.verticallyResizable = YES;
    _rulesTextView.horizontallyResizable = NO;
    _rulesTextView.autoresizingMask = NSViewWidthSizable;
    _rulesTextView.textContainer.widthTracksTextView = YES;
    NSScrollView* rulesScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    rulesScroll.documentView = _rulesTextView;
    rulesScroll.hasVerticalScroller = YES;
    rulesScroll.hasHorizontalScroller = NO;
    rulesScroll.borderType = NSNoBorder;
    _rulesEditorFrame = rounded_editor_box(rulesScroll);
    _rulesStatus = label(@"Select a Profile to edit its Rules.", @"Rules validation status");
    _rulesStatus.lineBreakMode = NSLineBreakByTruncatingTail;
    NSStackView* rulesStack = [NSStackView stackViewWithViews:@[
        rulesHeading, rulesActions, _rulesEditorFrame, _rulesStatus]];
    rulesStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    rulesStack.alignment = NSLayoutAttributeLeading;
    rulesStack.spacing = 10.0;
    [rulesActions.widthAnchor constraintEqualToAnchor:rulesStack.widthAnchor].active = YES;
    [_rulesEditorFrame.widthAnchor constraintEqualToAnchor:rulesStack.widthAnchor].active = YES;
    [_rulesEditorFrame setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_rulesEditorFrame setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    _rulesView = rulesStack;

    _settingsFieldControls = [NSMutableArray array];
    NSMutableArray<NSView*>* settingsFieldRows = [NSMutableArray array];
    for (const auto& descriptor : ccs::application_field_descriptors()) {
        const auto name = field_display_name(descriptor.display_name_key);
        NSControl* input = configuration_field_control(
            descriptor, self, @selector(settingsFieldChanged:));
        input.identifier = ns_string(std::string(descriptor.key));
        if ([input isKindOfClass:NSTextField.class]) {
            static_cast<NSTextField*>(input).delegate = self;
        }
        [_settingsFieldControls addObject:@{
            @"key": ns_string(std::string(descriptor.key)),
            @"input": input,
            @"kind": @(static_cast<NSInteger>(descriptor.input_kind)),
            @"required": @(descriptor.required),
            @"name": name,
        }];
        NSTextField* fieldLabel = label(name, name);
        NSStackView* row = [NSStackView stackViewWithViews:@[fieldLabel, input]];
        row.orientation = NSUserInterfaceLayoutOrientationHorizontal;
        row.alignment = NSLayoutAttributeCenterY;
        row.distribution = NSStackViewDistributionFill;
        row.spacing = 14.0;
        [fieldLabel.widthAnchor constraintEqualToAnchor:input.widthAnchor
            multiplier:(32.0 / 68.0)].active = YES;
        [fieldLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [input setContentHuggingPriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [input setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
            forOrientation:NSLayoutConstraintOrientationHorizontal];
        [settingsFieldRows addObject:row];
    }
    _settingsFieldRows = [settingsFieldRows copy];
    NSStackView* settingsFieldsStack = [NSStackView stackViewWithViews:settingsFieldRows];
    settingsFieldsStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    settingsFieldsStack.alignment = NSLayoutAttributeLeading;
    settingsFieldsStack.distribution = NSStackViewDistributionFill;
    settingsFieldsStack.spacing = 10.0;
    CCSFlippedView* settingsDocument = [[CCSFlippedView alloc]
        initWithFrame:NSZeroRect];
    settingsDocument.translatesAutoresizingMaskIntoConstraints = NO;
    settingsFieldsStack.translatesAutoresizingMaskIntoConstraints = NO;
    [settingsDocument addSubview:settingsFieldsStack];
    [NSLayoutConstraint activateConstraints:@[
        [settingsFieldsStack.leadingAnchor constraintEqualToAnchor:settingsDocument.leadingAnchor constant:8.0],
        [settingsFieldsStack.trailingAnchor constraintEqualToAnchor:settingsDocument.trailingAnchor constant:-12.0],
        [settingsFieldsStack.topAnchor constraintEqualToAnchor:settingsDocument.topAnchor constant:8.0],
        [settingsDocument.bottomAnchor constraintEqualToAnchor:settingsFieldsStack.bottomAnchor
            constant:8.0],
    ]];
    for (NSView* row in settingsFieldRows) {
        [row.widthAnchor constraintEqualToAnchor:settingsFieldsStack.widthAnchor].active = YES;
    }
    NSScrollView* settingsScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    settingsScroll.documentView = settingsDocument;
    settingsScroll.hasVerticalScroller = YES;
    settingsScroll.hasHorizontalScroller = NO;
    settingsScroll.autohidesScrollers = YES;
    settingsScroll.borderType = NSNoBorder;
    [settingsDocument.leadingAnchor constraintEqualToAnchor:settingsScroll.contentView.leadingAnchor].active = YES;
    [settingsDocument.topAnchor constraintEqualToAnchor:settingsScroll.contentView.topAnchor].active = YES;
    [settingsDocument.widthAnchor constraintEqualToAnchor:settingsScroll.contentView.widthAnchor].active = YES;
    _updateSettingsButton = push_button(
        @"Update Settings", self, @selector(updateSettings:), @"Update application Settings");
    NSTextField* settingsHeading = label(@"Settings", @"Settings editor heading");
    settingsHeading.font = [NSFont systemFontOfSize:17.0 weight:NSFontWeightSemibold];
    NSStackView* settingsHeader = [NSStackView stackViewWithViews:@[
        settingsHeading, _updateSettingsButton]];
    settingsHeader.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    settingsHeader.alignment = NSLayoutAttributeCenterY;
    settingsHeader.distribution = NSStackViewDistributionFill;
    [settingsHeading setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1.0
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* settingsStack = [NSStackView stackViewWithViews:@[
        settingsHeader, settingsScroll]];
    settingsStack.orientation = NSUserInterfaceLayoutOrientationVertical;
    settingsStack.alignment = NSLayoutAttributeLeading;
    settingsStack.distribution = NSStackViewDistributionFill;
    settingsStack.spacing = 10.0;
    [settingsHeader.widthAnchor constraintEqualToAnchor:settingsStack.widthAnchor].active = YES;
    [settingsScroll.widthAnchor constraintEqualToAnchor:settingsStack.widthAnchor].active = YES;
    [settingsScroll setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    [settingsScroll setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    _settingsView = settingsStack;

    _editorHost = [[NSView alloc] initWithFrame:NSZeroRect];
    _editorHost.identifier = @"Editor host layout";
    for (NSView* editor in @[_profilesView, _rulesView, _settingsView]) {
        editor.translatesAutoresizingMaskIntoConstraints = NO;
        [_editorHost addSubview:editor];
        [NSLayoutConstraint activateConstraints:@[
            [editor.leadingAnchor constraintEqualToAnchor:_editorHost.leadingAnchor],
            [editor.trailingAnchor constraintEqualToAnchor:_editorHost.trailingAnchor],
            [editor.topAnchor constraintEqualToAnchor:_editorHost.topAnchor],
            [editor.bottomAnchor constraintEqualToAnchor:_editorHost.bottomAnchor],
        ]];
    }
    NSBox* navigationSeparator = [[NSBox alloc] initWithFrame:NSZeroRect];
    navigationSeparator.boxType = NSBoxSeparator;
    _workspace = [NSStackView stackViewWithViews:@[
        _navigation, navigationSeparator, _editorHost]];
    _workspace.identifier = @"Workspace layout";
    _workspace.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _workspace.alignment = NSLayoutAttributeTop;
    _workspace.distribution = NSStackViewDistributionFill;
    _workspace.spacing = 16.0;
    [_navigation.widthAnchor constraintEqualToAnchor:_editorHost.widthAnchor
        multiplier:kNavigationEditorRatio].active = YES;
    [navigationSeparator.widthAnchor constraintEqualToConstant:1.0].active = YES;
    [navigationSeparator.heightAnchor constraintEqualToAnchor:_workspace.heightAnchor].active = YES;
    [_editorHost.heightAnchor constraintEqualToAnchor:_workspace.heightAnchor].active = YES;
    [_navigation setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_editorHost setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_editorHost setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];

    _commandStatus = label(@"", @"Command status");
    _commandStatus.lineBreakMode = NSLineBreakByTruncatingTail;
    _reloadDraftButton = push_button(
        @"Reload draft", self, @selector(reloadDraft:), @"Reload configuration from disk");
    _discardButton = push_button(@"Discard", self, @selector(discardDraft:), @"Discard changes");
    _applyButton = push_button(@"Apply changes", self, @selector(applyDraft:), @"Apply changes");
    NSStackView* footerActions = [NSStackView stackViewWithViews:@[
        _reloadDraftButton, _discardButton, _applyButton]];
    footerActions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    footerActions.alignment = NSLayoutAttributeCenterY;
    footerActions.spacing = 8.0;
    [footerActions setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [footerActions setContentCompressionResistancePriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSView* footer = [[NSView alloc] initWithFrame:NSZeroRect];
    _commandStatus.translatesAutoresizingMaskIntoConstraints = NO;
    footerActions.translatesAutoresizingMaskIntoConstraints = NO;
    [footer addSubview:_commandStatus];
    [footer addSubview:footerActions];
    [NSLayoutConstraint activateConstraints:@[
        [_commandStatus.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor],
        [_commandStatus.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
        [_commandStatus.trailingAnchor constraintLessThanOrEqualToAnchor:footerActions.leadingAnchor
            constant:-8.0],
        [footerActions.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor],
        [footerActions.topAnchor constraintEqualToAnchor:footer.topAnchor],
        [footerActions.bottomAnchor constraintEqualToAnchor:footer.bottomAnchor],
    ]];
    [_commandStatus setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_commandStatus setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_reloadDraftButton setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_applyButton setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_discardButton setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSBox* headerSeparator = [[NSBox alloc] initWithFrame:NSZeroRect];
    headerSeparator.boxType = NSBoxSeparator;
    NSBox* footerSeparator = [[NSBox alloc] initWithFrame:NSZeroRect];
    footerSeparator.boxType = NSBoxSeparator;
    NSStackView* root = [NSStackView stackViewWithViews:@[
        _serviceRow, headerSeparator, _workspace, footerSeparator, footer]];
    root.identifier = @"Main window root layout";
    root.translatesAutoresizingMaskIntoConstraints = NO;
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.distribution = NSStackViewDistributionFill;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 10.0;
    NSView* content = [[NSView alloc] initWithFrame:NSZeroRect];
    content.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    window.contentView = content;
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16.0],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16.0],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor constant:16.0],
        [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16.0],
        [_serviceRow.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [headerSeparator.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [_workspace.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [footerSeparator.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [footer.widthAnchor constraintEqualToAnchor:root.widthAnchor],
    ]];
    [_workspace setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_workspace setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    [_workspace.heightAnchor constraintGreaterThanOrEqualToConstant:340.0].active = YES;

    _localStatus = @"";
    [self showView:CCSMainWindowViewProfiles];
    window.defaultButtonCell = _applyButton.cell;
    [self configureKeyLoop];
    return self;
}

- (void)dealloc {
    if (_resourceCounted) {
        --live_controller_count;
    }
}

- (void)updateResponsiveLayout {
    [self.window.contentView setNeedsUpdateConstraints:YES];
    [self.window.contentView updateConstraintsForSubtreeIfNeeded];
    [self.window.contentView setNeedsLayout:YES];
    [self.window.contentView layoutSubtreeIfNeeded];
    const NSRect clipBounds = _profileTableScroll.contentView.bounds;
    const CGFloat clipWidth = NSWidth(clipBounds);
    if (clipWidth < 1.0) {
        return;
    }

    NSRect tableFrame = _profileTable.frame;
    tableFrame.size.width = clipWidth;
    tableFrame.size.height = std::max(NSHeight(tableFrame), NSHeight(clipBounds));
    _profileTable.frame = tableFrame;

    const CGFloat spacing = _profileTable.intercellSpacing.width;
    const CGFloat available = std::max(0.0, clipWidth - (2.0 * spacing));
    const CGFloat profileWidth = std::floor(available * kProfileColumnRatio);
    const CGFloat enabledWidth = std::floor(available * kEnabledColumnRatio);
    const CGFloat readinessWidth = available - profileWidth - enabledWidth;
    _profileColumn.width = profileWidth;
    _enabledColumn.width = enabledWidth;
    _readinessColumn.width = readinessWidth;
    [_profileTable.headerView setNeedsDisplay:YES];
    [_profileTable setNeedsDisplay:YES];
}

- (void)configureKeyLoop {
    NSMutableArray<NSView*>* controls = [NSMutableArray arrayWithArray:@[
        _startButton, _stopButton, _reloadButton, _lightweightCheckbox,
        _profilesNavigationButton, _rulesNavigationButton, _settingsNavigationButton,
    ]];
    if (_currentView == CCSMainWindowViewProfiles) {
        [controls addObjectsFromArray:@[
            _profileTable, _newProfileField, _addButton, _removeButton,
            _renameField, _renameButton, _enabledCheckbox,
        ]];
        for (NSDictionary* entry in _profileFieldControls) {
            [controls addObject:entry[@"input"]];
        }
        [controls addObject:_updateProfileFieldsButton];
    } else if (_currentView == CCSMainWindowViewRules) {
        [controls addObjectsFromArray:@[
            _rulesProfilePopup, _rulesTextView, _formatRulesButton, _updateRulesButton,
        ]];
    } else {
        for (NSDictionary* entry in _settingsFieldControls) {
            [controls addObject:entry[@"input"]];
        }
        [controls addObject:_updateSettingsButton];
    }
    [controls addObjectsFromArray:@[_reloadDraftButton, _discardButton, _applyButton]];
    for (NSUInteger index = 0; index < controls.count; ++index) {
        NSView* current = controls[index];
        current.nextKeyView = controls[(index + 1) % controls.count];
    }
}

- (void)prepareForDisplay:(BOOL)resetScroll {
    [self.window.contentView layoutSubtreeIfNeeded];
    [self updateResponsiveLayout];
    [_profileDetailsContent layoutSubtreeIfNeeded];
    [_profileDetailsDocument layoutSubtreeIfNeeded];
    if (resetScroll) {
        NSClipView* clip = _profileDetailsScroll.contentView;
        [clip scrollToPoint:NSZeroPoint];
        [_profileDetailsScroll reflectScrolledClipView:clip];
    }
    [self.window.contentView layoutSubtreeIfNeeded];
    [self updateResponsiveLayout];
}

- (void)showView:(CCSMainWindowView)view {
    _currentView = view;
    _profilesView.hidden = view != CCSMainWindowViewProfiles;
    _rulesView.hidden = view != CCSMainWindowViewRules;
    _settingsView.hidden = view != CCSMainWindowViewSettings;
    _profilesNavigationButton.state = view == CCSMainWindowViewProfiles
        ? NSControlStateValueOn : NSControlStateValueOff;
    _rulesNavigationButton.state = view == CCSMainWindowViewRules
        ? NSControlStateValueOn : NSControlStateValueOff;
    _settingsNavigationButton.state = view == CCSMainWindowViewSettings
        ? NSControlStateValueOn : NSControlStateValueOff;
    const auto styleNavigationButton = [](NSButton* button, BOOL selected) {
        NSMutableAttributedString* title = [[NSMutableAttributedString alloc]
            initWithString:button.title];
        [title addAttribute:NSForegroundColorAttributeName
            value:(selected ? NSColor.controlAccentColor : NSColor.secondaryLabelColor)
            range:NSMakeRange(0, title.length)];
        button.attributedTitle = title;
    };
    styleNavigationButton(
        _profilesNavigationButton, view == CCSMainWindowViewProfiles);
    styleNavigationButton(
        _rulesNavigationButton, view == CCSMainWindowViewRules);
    styleNavigationButton(
        _settingsNavigationButton, view == CCSMainWindowViewSettings);
    [self.window.contentView layoutSubtreeIfNeeded];
    [self updateResponsiveLayout];
    [self configureKeyLoop];
}

- (CCSMainWindowView)currentView {
    return _currentView;
}

- (void)invalidateOwner {
    _owner = nullptr;
    self.window.delegate = nil;
    _profileTable.dataSource = nil;
    _profileTable.delegate = nil;
    _rulesTextView.delegate = nil;
    for (NSDictionary* entry in _profileFieldControls) {
        NSControl* input = entry[@"input"];
        if ([input isKindOfClass:NSTextField.class]) {
            static_cast<NSTextField*>(input).delegate = nil;
        }
    }
    for (NSDictionary* entry in _settingsFieldControls) {
        NSControl* input = entry[@"input"];
        if ([input isKindOfClass:NSTextField.class]) {
            static_cast<NSTextField*>(input).delegate = nil;
        }
    }
}

- (BOOL)hasLocalEdits {
    return _profileLocalDirty || _rulesLocalDirty || _settingsLocalDirty
        || _profileUpdateAwaiting || _rulesUpdateAwaiting || _settingsUpdateAwaiting;
}

- (void)discardLocalEdits {
    _profileLocalDirty = NO;
    _rulesLocalDirty = NO;
    _settingsLocalDirty = NO;
    _profileUpdateAwaiting = NO;
    _rulesUpdateAwaiting = NO;
    _settingsUpdateAwaiting = NO;
    _hasProfileLocalKey = NO;
    _hasRulesLocalKey = NO;
    _localStatus = @"";
    _localStatusIsError = NO;
    if (_owner != nullptr) {
        [self render];
    }
}

- (BOOL)confirmDiscardLocalEdits {
    if (![self hasLocalEdits]) {
        return YES;
    }
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Unsubmitted editor changes";
    alert.informativeText = @"Discard changes that have not been updated to the draft?";
    [alert addButtonWithTitle:@"Discard local edits"];
    [alert addButtonWithTitle:@"Keep editing"];
    if ([alert runModal] != NSAlertFirstButtonReturn) {
        return NO;
    }
    [self discardLocalEdits];
    return YES;
}

- (BOOL)confirmDiscardProfileEditors {
    if (!_profileLocalDirty && !_rulesLocalDirty
        && !_profileUpdateAwaiting && !_rulesUpdateAwaiting) {
        return YES;
    }
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Unsubmitted Profile editor changes";
    alert.informativeText = @"Discard local Profile or Rules edits before changing selection?";
    [alert addButtonWithTitle:@"Discard local edits"];
    [alert addButtonWithTitle:@"Keep editing"];
    if ([alert runModal] != NSAlertFirstButtonReturn) {
        return NO;
    }
    _profileLocalDirty = NO;
    _rulesLocalDirty = NO;
    _profileUpdateAwaiting = NO;
    _rulesUpdateAwaiting = NO;
    _hasProfileLocalKey = NO;
    _hasRulesLocalKey = NO;
    return YES;
}

- (void)resolvePendingEditorUpdates {
    if (_owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state == nullptr || state->command_pending || !state->last_command) {
        return;
    }
    const auto& result = *state->last_command;
    if (_profileUpdateAwaiting
        && result.sequence > _profileUpdatePreviousSequence
        && result.command == ccs::MainWindowCommand::UpdateProfileFields) {
        _profileUpdateAwaiting = NO;
        if (result.succeeded()) {
            _profileLocalDirty = NO;
            _hasProfileLocalKey = NO;
        }
    }
    if (_rulesUpdateAwaiting
        && result.sequence > _rulesUpdatePreviousSequence
        && (result.command == ccs::MainWindowCommand::ReplaceRulesText
            || result.command == ccs::MainWindowCommand::FormatRulesText)) {
        _rulesUpdateAwaiting = NO;
        if (result.succeeded()) {
            _rulesLocalDirty = NO;
            _hasRulesLocalKey = NO;
        }
    }
    if (_settingsUpdateAwaiting
        && result.sequence > _settingsUpdatePreviousSequence
        && result.command == ccs::MainWindowCommand::UpdateApplicationFields) {
        _settingsUpdateAwaiting = NO;
        if (result.succeeded()) {
            _settingsLocalDirty = NO;
        }
    }
}

- (void)populateFieldControls:(NSArray<NSDictionary<NSString*, id>*>*)entries
    states:(const std::vector<ccs::ConfigurationFieldState>&)states {
    for (NSDictionary* entry in entries) {
        const auto key = utf8_string(entry[@"key"]);
        const auto* field = find_field_state(states, key);
        if (field == nullptr) {
            continue;
        }
        NSControl* input = entry[@"input"];
        if (field->input_kind == ccs::ConfigurationFieldInputKind::Boolean) {
            const bool checked = field->value
                && std::holds_alternative<bool>(*field->value)
                && std::get<bool>(*field->value);
            static_cast<NSButton*>(input).state = checked
                ? NSControlStateValueOn : NSControlStateValueOff;
        } else if (field->input_kind == ccs::ConfigurationFieldInputKind::Enumeration) {
            NSPopUpButton* popup = static_cast<NSPopUpButton*>(input);
            if (!field->value && !field->required) {
                [popup selectItemAtIndex:0];
            } else if (field->value) {
                [popup selectItemWithTitle:field_value_text(*field->value)];
            }
        } else {
            NSTextField* text = static_cast<NSTextField*>(input);
            text.stringValue = field->value ? field_value_text(*field->value) : @"";
        }
    }
}

- (BOOL)collectFieldEdits:(NSArray<NSDictionary<NSString*, id>*>*)entries
    states:(const std::vector<ccs::ConfigurationFieldState>&)states
    edits:(std::vector<ccs::ConfigurationFieldEdit>&)edits
    error:(std::string&)error {
    edits.clear();
    error.clear();
    edits.reserve(entries.count);
    for (NSDictionary* entry in entries) {
        const auto key = utf8_string(entry[@"key"]);
        const auto* field = find_field_state(states, key);
        if (field == nullptr) {
            error = "field is not present in the current snapshot: " + key;
            return NO;
        }
        NSControl* input = entry[@"input"];
        std::string raw;
        if (field->input_kind == ccs::ConfigurationFieldInputKind::Boolean) {
            raw = static_cast<NSButton*>(input).state == NSControlStateValueOn
                ? "true" : "false";
        } else if (field->input_kind == ccs::ConfigurationFieldInputKind::Enumeration) {
            NSPopUpButton* popup = static_cast<NSPopUpButton*>(input);
            const NSInteger index = popup.indexOfSelectedItem;
            if (!field->required && index == 0) {
                if (!field->value) {
                    continue;
                }
                edits.push_back({key, std::nullopt});
                continue;
            }
            raw = utf8_string(popup.titleOfSelectedItem);
        } else {
            raw = utf8_string(static_cast<NSTextField*>(input).stringValue);
            if (raw.empty() && !field->required) {
                if (!field->value) {
                    continue;
                }
                edits.push_back({key, std::nullopt});
                continue;
            }
        }
        const auto* descriptor = ccs::find_configuration_field_descriptor(
            field->scope, key);
        ccs::ConfigurationFieldValue value;
        std::string parse_error;
        if (descriptor == nullptr
            || !ccs::parse_configuration_field_value(*descriptor, raw, value, parse_error)) {
            error = utf8_string(entry[@"name"]);
            error += ": ";
            error += parse_error;
            [self.window makeFirstResponder:input];
            return NO;
        }
        if (field->value && *field->value == value) {
            continue;
        }
        edits.push_back({key, std::move(value)});
    }
    return YES;
}

- (void)profileFieldChanged:(id)sender {
    (void)sender;
    if (_updating || _owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state && state->profile_editor) {
        _profileLocalDirty = YES;
        _hasProfileLocalKey = YES;
        _profileLocalKey = state->profile_editor->key;
        [self updateEnabledStates];
    }
}

- (void)settingsFieldChanged:(id)sender {
    (void)sender;
    if (_updating || _owner == nullptr) {
        return;
    }
    _settingsLocalDirty = YES;
    [self updateEnabledStates];
}

- (void)controlTextDidChange:(NSNotification*)notification {
    if (_updating) {
        return;
    }
    id object = notification.object;
    for (NSDictionary* entry in _profileFieldControls) {
        if (entry[@"input"] == object) {
            [self profileFieldChanged:object];
            return;
        }
    }
    for (NSDictionary* entry in _settingsFieldControls) {
        if (entry[@"input"] == object) {
            [self settingsFieldChanged:object];
            return;
        }
    }
}

- (void)textDidChange:(NSNotification*)notification {
    if (!_updating && notification.object == _rulesTextView && _owner != nullptr) {
        const auto state = _owner->state();
        if (state && state->rules_editor) {
            _rulesLocalDirty = YES;
            _hasRulesLocalKey = YES;
            _rulesLocalKey = state->rules_editor->profile_key;
            [self updateEnabledStates];
        }
    }
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    (void)tableView;
    const auto state = _owner == nullptr ? nullptr : _owner->state();
    return state == nullptr ? 0 : static_cast<NSInteger>(state->profiles.size());
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
    row:(NSInteger)row {
    const auto state = _owner == nullptr ? nullptr : _owner->state();
    if (state == nullptr || row < 0 || static_cast<std::size_t>(row) >= state->profiles.size()) {
        return nil;
    }
    const auto& profile = state->profiles[static_cast<std::size_t>(row)];
    NSTextField* field = [tableView makeViewWithIdentifier:tableColumn.identifier owner:self];
    if (field == nil) {
        field = [NSTextField labelWithString:@""];
        field.identifier = tableColumn.identifier;
        field.lineBreakMode = NSLineBreakByTruncatingMiddle;
    }
    if ([tableColumn.identifier isEqualToString:@"profile"]) {
        field.stringValue = ns_string(profile.id);
        field.accessibilityLabel = @"Profile ID";
    } else if ([tableColumn.identifier isEqualToString:@"enabled"]) {
        field.stringValue = profile.enabled ? @"Yes" : @"No";
        field.accessibilityLabel = @"Profile enabled state";
    } else {
        field.stringValue = readiness_text(profile.readiness);
        field.accessibilityLabel = @"Profile readiness";
    }
    return field;
}

- (void)tableViewSelectionDidChange:(NSNotification*)notification {
    (void)notification;
    if (_updating || _owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    const NSInteger row = _profileTable.selectedRow;
    if (state == nullptr || row < 0 || static_cast<std::size_t>(row) >= state->profiles.size()) {
        return;
    }
    const auto& profile = state->profiles[static_cast<std::size_t>(row)];
    if (state->selected_profile_key && *state->selected_profile_key == profile.key) {
        return;
    }
    if (![self confirmDiscardProfileEditors]) {
        _updating = YES;
        [self render];
        _updating = NO;
        return;
    }
    ccs::MainWindowCommandRequest request{
        ccs::MainWindowCommand::SelectProfile, profile.id};
    request.profile_key = profile.key;
    _owner->submit(std::move(request));
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_owner != nullptr) {
        _owner->request_window_close();
    }
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    [self updateResponsiveLayout];
}

- (void)render {
    if (_owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state == nullptr) {
        return;
    }
    [self resolvePendingEditorUpdates];
    _updating = YES;
    const auto application_state = state->application.state;
    _serviceStatus.stringValue = ns_string(ccs::application_state_name(application_state));
    _serviceStatus.textColor = service_status_text_color(application_state);
    _serviceStatusBadge.fillColor = service_status_background_color(application_state);
    if (state->application.listener_host.empty() || state->application.listener_port == 0) {
        _listenerStatus.stringValue = @"Listener inactive";
    } else {
        _listenerStatus.stringValue = [NSString stringWithFormat:@"%s:%u",
            state->application.listener_host.c_str(),
            static_cast<unsigned int>(state->application.listener_port)];
    }
    _lightweightCheckbox.state = state->lightweight_mode
        ? NSControlStateValueOn
        : NSControlStateValueOff;
    [_profileTable reloadData];
    NSInteger selected_row = -1;
    if (state->selected_profile_key) {
        for (std::size_t index = 0; index < state->profiles.size(); ++index) {
            if (state->profiles[index].key == *state->selected_profile_key) {
                selected_row = static_cast<NSInteger>(index);
                break;
            }
        }
    } else if (state->selected_profile_id) {
        for (std::size_t index = 0; index < state->profiles.size(); ++index) {
            if (state->profiles[index].id == *state->selected_profile_id) {
                selected_row = static_cast<NSInteger>(index);
                break;
            }
        }
    }
    if (selected_row >= 0) {
        [_profileTable selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(selected_row)]
            byExtendingSelection:NO];
        [_profileTable scrollRowToVisible:selected_row];
    } else {
        [_profileTable deselectAll:nil];
    }

    const auto* profile = _owner->selected_profile();
    if (profile == nullptr) {
        if (!_profileLocalDirty) {
            _renameField.stringValue = @"";
            _enabledCheckbox.state = NSControlStateValueOff;
        }
        _protocolValue.stringValue = @"No Profile selected";
        _readinessValue.stringValue = @"";
        _rulesValue.stringValue = @"";
        if (!_profileLocalDirty) {
            _profileDetail.stringValue = @"Select or create a Profile.";
        }
    } else {
        if (!_profileLocalDirty) {
            if (utf8_string(_renameField.stringValue) != profile->id) {
                _renameField.stringValue = ns_string(profile->id);
            }
            _enabledCheckbox.state = profile->enabled
                ? NSControlStateValueOn
                : NSControlStateValueOff;
            if (state->profile_editor) {
                [self populateFieldControls:_profileFieldControls
                    states:state->profile_editor->fields];
            }
        }
        _protocolValue.stringValue = profile->protocol
            ? ns_string(*profile->protocol)
            : @"Not configured";
        _readinessValue.stringValue = readiness_text(profile->readiness);
        _rulesValue.stringValue = [NSString stringWithFormat:@"%zu enabled / %zu total",
            profile->enabled_rule_count,
            profile->rule_count];
        _profileDetail.stringValue = ns_string(profile->status_detail);
    }

    [_rulesProfilePopup removeAllItems];
    NSInteger selected_rule_index = -1;
    for (std::size_t index = 0; index < state->profiles.size(); ++index) {
        const auto& item = state->profiles[index];
        [_rulesProfilePopup addItemWithTitle:ns_string(item.id)];
        [_rulesProfilePopup itemAtIndex:static_cast<NSInteger>(index)].representedObject =
            @(static_cast<long long>(item.key));
        if (state->rules_editor && item.key == state->rules_editor->profile_key) {
            selected_rule_index = static_cast<NSInteger>(index);
        }
    }
    if (selected_rule_index >= 0) {
        [_rulesProfilePopup selectItemAtIndex:selected_rule_index];
    }
    if (state->rules_editor) {
        if (!_rulesLocalDirty) {
            _rulesTextView.string = ns_string(canonical_newlines(state->rules_editor->text));
        }
        if (state->rules_editor->diagnostic) {
            const auto& diagnostic = *state->rules_editor->diagnostic;
            _rulesStatus.stringValue = [NSString stringWithFormat:@"Line %zu, column %zu: %@",
                diagnostic.line, diagnostic.column, ns_string(diagnostic.message)];
            _rulesStatus.textColor = NSColor.systemRedColor;
        } else {
            _rulesStatus.stringValue = @"Canonical ccs-trans.rules/v1 JSON";
            _rulesStatus.textColor = NSColor.secondaryLabelColor;
        }
    } else {
        if (!_rulesLocalDirty) {
            _rulesTextView.string = @"";
        }
        _rulesStatus.stringValue = @"Select a Profile to edit its Rules.";
        _rulesStatus.textColor = NSColor.secondaryLabelColor;
    }
    if (!_settingsLocalDirty && !_settingsUpdateAwaiting) {
        [self populateFieldControls:_settingsFieldControls states:state->application_fields];
    }

    if (_localStatus.length > 0) {
        [_commandStatus setStringValue:_localStatus];
        _commandStatus.textColor = _localStatusIsError
            ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    } else if (state->command_pending) {
        [self setLocalStatus:@"Working..." error:NO];
    } else if (state->last_command) {
        const auto& result = *state->last_command;
        if (!result.detail.empty()) {
            [self setLocalStatus:ns_string(result.detail) error:!result.succeeded()];
        } else if (result.outcome == ccs::CommandOutcome::SavedPendingRuntimeApply) {
            [self setLocalStatus:@"Saved; runtime update is still pending" error:YES];
        } else {
            [self setLocalStatus:(result.succeeded() ? @"Completed" : @"Command failed")
                error:!result.succeeded()];
        }
    } else {
        [self setLocalStatus:@"" error:NO];
    }

    [self updateEnabledStates];
    _updating = NO;
}

- (void)setLocalStatus:(NSString*)message error:(BOOL)error {
    _localStatus = message == nil ? @"" : message;
    _localStatusIsError = error;
    _commandStatus.stringValue = _localStatus;
    _commandStatus.textColor = error ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

- (void)updateEnabledStates {
    if (_owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state == nullptr) {
        return;
    }
    const bool pending = state->command_pending;
    const auto service_actions = ccs::service_actions_for(state->application.state);
    _startButton.enabled = !pending && service_actions.can_start;
    _stopButton.enabled = !pending && service_actions.can_stop;
    _reloadButton.enabled = !pending && service_actions.can_reload;
    _lightweightCheckbox.enabled = !pending;
    _profilesNavigationButton.enabled = !pending;
    _rulesNavigationButton.enabled = !pending;
    _settingsNavigationButton.enabled = !pending;
    _profileTable.enabled = !pending;
    _newProfileField.enabled = !pending;
    _addButton.enabled = !pending;
    const bool has_profile = _owner->selected_profile() != nullptr;
    _removeButton.enabled = !pending && has_profile;
    _renameField.enabled = !pending && has_profile;
    _renameButton.enabled = !pending && has_profile;
    _enabledCheckbox.enabled = !pending && has_profile;
    for (NSDictionary* entry in _profileFieldControls) {
        static_cast<NSControl*>(entry[@"input"]).enabled = !pending && has_profile;
    }
    _updateProfileFieldsButton.enabled = !pending && has_profile;
    _rulesProfilePopup.enabled = !pending && !state->profiles.empty();
    _rulesTextView.editable = !pending && has_profile;
    _formatRulesButton.enabled = !pending && has_profile;
    _updateRulesButton.enabled = !pending && has_profile;
    for (NSDictionary* entry in _settingsFieldControls) {
        static_cast<NSControl*>(entry[@"input"]).enabled = !pending;
    }
    _updateSettingsButton.enabled = !pending && _settingsFieldControls.count > 0;
    _reloadDraftButton.enabled = !pending && state->draft.loaded() && !state->draft.busy();
    const bool local_dirty = _profileLocalDirty || _rulesLocalDirty || _settingsLocalDirty
        || _profileUpdateAwaiting || _rulesUpdateAwaiting || _settingsUpdateAwaiting;
    _applyButton.enabled = !pending && state->draft.dirty() && !local_dirty;
    _discardButton.enabled = !pending && (state->draft.dirty() || local_dirty);
}

- (BOOL)validateResponsiveLayout:(std::string&)error {
    [self.window.contentView layoutSubtreeIfNeeded];
    [self updateResponsiveLayout];

    const NSSize minimum = self.window.minSize;
    if (!approximately_equal(minimum.width, kMinimumWindowWidth)
        || !approximately_equal(minimum.height, kMinimumWindowHeight)) {
        error = "main window minimum size does not match the responsive layout contract";
        return NO;
    }

    const CGFloat headerWidth = NSWidth(_serviceRow.bounds);
    if (headerWidth < 1.0
        || !approximately_equal(
            NSWidth(_headerBrandRegion.frame), headerWidth * kHeaderBrandRatio)
        || !approximately_equal(
            NSWidth(_headerStatusRegion.frame), headerWidth * kHeaderStatusRatio)
        || !approximately_equal(
            NSWidth(_headerActionsRegion.frame), headerWidth * kHeaderActionsRatio)) {
        error = "header regions do not follow the 18:34:48 width contract";
        return NO;
    }
    if (!approximately_equal(
            NSMaxX(_serviceActions.frame), NSMaxX(_headerActionsRegion.bounds))) {
        error = "header actions are not aligned to the trailing edge of their region";
        return NO;
    }
    for (NSView* action in _serviceActions.arrangedSubviews) {
        const NSSize intrinsic = action.intrinsicContentSize;
        if (intrinsic.width > 0.0
            && !approximately_equal(NSWidth(action.frame), intrinsic.width, 2.0)) {
            error = "a header action changed from its intrinsic width";
            return NO;
        }
    }

    const CGFloat navigationWidth = NSWidth(_navigation.frame);
    const CGFloat editorWidth = NSWidth(_editorHost.frame);
    if (navigationWidth < 1.0 || editorWidth < 1.0
        || !approximately_equal(
            navigationWidth, editorWidth * kNavigationEditorRatio)) {
        error = "navigation and editor do not follow the 16:84 width contract";
        return NO;
    }
    for (NSButton* button in @[
             _profilesNavigationButton, _rulesNavigationButton, _settingsNavigationButton]) {
        const NSRect alignmentRect = [button alignmentRectForFrame:button.frame];
        if (!approximately_equal(NSHeight(alignmentRect), kNavigationButtonHeight)) {
            error = "a navigation button does not have the required aligned height: "
                + std::to_string(NSHeight(alignmentRect));
            return NO;
        }
    }
    if (!approximately_equal(_navigation.spacing, kNavigationButtonSpacing)
        || !approximately_equal(NSHeight(_navigation.bounds), kNavigationStackHeight)) {
        error = "navigation spacing or total height does not match its layout contract";
        return NO;
    }

    const CGFloat profileListWidth = NSWidth(_profilesList.frame);
    const CGFloat profileDetailsWidth = NSWidth(_profileDetailsScroll.frame);
    if (profileListWidth < 1.0 || profileDetailsWidth < 1.0
        || !approximately_equal(
            profileListWidth, profileDetailsWidth * kProfileListDetailsRatio)) {
        error = "Profiles list and details do not follow the 38:62 width contract";
        return NO;
    }

    if (_currentView == CCSMainWindowViewProfiles) {
        const CGFloat clipWidth = NSWidth(_profileTableScroll.contentView.bounds);
        const CGFloat spacing = _profileTable.intercellSpacing.width;
        const CGFloat available = clipWidth - (2.0 * spacing);
        const CGFloat columnTotal = _profileColumn.width
            + _enabledColumn.width + _readinessColumn.width;
        if (available < 1.0
            || !approximately_equal(columnTotal, available)
            || !approximately_equal(
                _profileColumn.width, available * kProfileColumnRatio, 1.5)
            || !approximately_equal(
                _enabledColumn.width, available * kEnabledColumnRatio, 1.5)
            || !approximately_equal(
                _readinessColumn.width, available * kReadinessColumnRatio, 1.5)) {
            error = "Profile table columns do not cover the visible width at 36:26:38";
            return NO;
        }
        for (NSTableColumn* column in @[
                 _profileColumn, _enabledColumn, _readinessColumn]) {
            if (column.width + 0.5 < column.headerCell.cellSize.width) {
                error = "a Profile table header is truncated";
                return NO;
            }
        }
    }

    NSArray<NSView*>* proportionalRows = _currentView == CCSMainWindowViewSettings
        ? _settingsFieldRows : (_currentView == CCSMainWindowViewProfiles
            ? _profileFieldRows : @[]);
    const CGFloat rowRatio = _currentView == CCSMainWindowViewSettings
        ? (32.0 / 68.0) : (34.0 / 66.0);
    for (NSView* view in proportionalRows) {
        NSStackView* row = static_cast<NSStackView*>(view);
        if (row.arrangedSubviews.count != 2) {
            error = "a proportional field row has an unexpected child count";
            return NO;
        }
        NSView* labelView = row.arrangedSubviews[0];
        NSView* inputView = row.arrangedSubviews[1];
        const CGFloat labelWidth = NSWidth(
            [labelView alignmentRectForFrame:labelView.frame]);
        const CGFloat inputWidth = NSWidth(
            [inputView alignmentRectForFrame:inputView.frame]);
        if (labelWidth < 1.0 || inputWidth < 1.0
            || !approximately_equal(labelWidth, inputWidth * rowRatio, 1.5)) {
            error = "a field row does not distribute its label and input proportionally: "
                + std::to_string(labelWidth) + ":" + std::to_string(inputWidth);
            return NO;
        }
        const NSRect labelAlignment = [labelView alignmentRectForFrame:labelView.frame];
        const NSRect inputAlignment = [inputView alignmentRectForFrame:inputView.frame];
        if (!approximately_equal(NSMinX(labelAlignment), NSMinX(row.bounds), 1.5)
            || !approximately_equal(NSMaxX(inputAlignment), NSMaxX(row.bounds), 1.5)) {
            error = "a proportional field row does not fill its available width";
            return NO;
        }
    }
    return YES;
}

- (BOOL)validateResponsiveBehavior:(std::string&)error {
    error.clear();
    const NSRect originalFrame = self.window.frame;
    const CCSMainWindowView originalView = _currentView;
    const auto restore = [&]() {
        [self.window setFrame:originalFrame display:YES];
        [self showView:originalView];
        [self updateResponsiveLayout];
    };
    const auto resize = [&](NSSize size) {
        NSScreen* screen = self.window.screen == nil ? NSScreen.mainScreen : self.window.screen;
        const NSRect visibleFrame = screen.visibleFrame;
        size.width = std::min(size.width, NSWidth(visibleFrame));
        size.height = std::min(size.height, NSHeight(visibleFrame));
        NSRect frame = NSMakeRect(
            NSMidX(visibleFrame) - (size.width / 2.0),
            NSMaxY(visibleFrame) - size.height,
            size.width,
            size.height);
        [self.window setFrame:frame display:YES];
        [self.window.contentView layoutSubtreeIfNeeded];
        [self updateResponsiveLayout];
    };
    const auto validate_views = [&]() -> BOOL {
        for (const auto view : {
                 CCSMainWindowViewProfiles,
                 CCSMainWindowViewRules,
                 CCSMainWindowViewSettings}) {
            [self showView:view];
            if (![self validateLayout:error]) {
                return NO;
            }
        }
        return YES;
    };

    resize(self.window.minSize);
    if (!validate_views()) {
        restore();
        return NO;
    }
    [self showView:CCSMainWindowViewProfiles];
    const CGFloat minimumNavigationWidth = NSWidth(_navigation.frame);
    const CGFloat minimumEditorWidth = NSWidth(_editorHost.frame);
    const CGFloat minimumListWidth = NSWidth(_profilesList.frame);
    const CGFloat minimumDetailsWidth = NSWidth(_profileDetailsScroll.frame);
    const CGFloat minimumProfileColumnWidth = _profileColumn.width;
    const CGFloat minimumEnabledColumnWidth = _enabledColumn.width;
    const CGFloat minimumReadinessColumnWidth = _readinessColumn.width;
    std::vector<NSSize> minimumActionSizes;
    for (NSView* action in _serviceActions.arrangedSubviews) {
        minimumActionSizes.push_back(action.frame.size);
    }

    resize(NSMakeSize(1200.0, 760.0));
    if (!validate_views()) {
        restore();
        return NO;
    }
    [self showView:CCSMainWindowViewProfiles];
    if (NSWidth(_navigation.frame) <= minimumNavigationWidth + 20.0
        || NSWidth(_editorHost.frame) <= minimumEditorWidth + 250.0
        || NSWidth(_profilesList.frame) <= minimumListWidth + 100.0
        || NSWidth(_profileDetailsScroll.frame) <= minimumDetailsWidth + 150.0
        || _profileColumn.width <= minimumProfileColumnWidth + 40.0
        || _enabledColumn.width <= minimumEnabledColumnWidth + 20.0
        || _readinessColumn.width <= minimumReadinessColumnWidth + 30.0) {
        error = "nested editor regions did not expand with the window: window/content/bounds/root/workspace/max "
            + std::to_string(NSWidth(self.window.frame)) + "/"
            + std::to_string(NSWidth(self.window.contentView.frame)) + "/"
            + std::to_string(NSWidth(self.window.contentView.bounds)) + "/"
            + std::to_string(NSWidth(_workspace.superview.frame)) + "/"
            + std::to_string(NSWidth(_workspace.frame)) + "/"
            + std::to_string(self.window.maxSize.width) + ", nav "
            + std::to_string(minimumNavigationWidth) + "->"
            + std::to_string(NSWidth(_navigation.frame)) + ", editor "
            + std::to_string(minimumEditorWidth) + "->"
            + std::to_string(NSWidth(_editorHost.frame)) + ", list "
            + std::to_string(minimumListWidth) + "->"
            + std::to_string(NSWidth(_profilesList.frame)) + ", details "
            + std::to_string(minimumDetailsWidth) + "->"
            + std::to_string(NSWidth(_profileDetailsScroll.frame)) + ", columns "
            + std::to_string(minimumProfileColumnWidth) + "/"
            + std::to_string(minimumEnabledColumnWidth) + "/"
            + std::to_string(minimumReadinessColumnWidth) + "->"
            + std::to_string(_profileColumn.width) + "/"
            + std::to_string(_enabledColumn.width) + "/"
            + std::to_string(_readinessColumn.width);
        restore();
        return NO;
    }
    NSUInteger actionIndex = 0;
    for (NSView* action in _serviceActions.arrangedSubviews) {
        const NSSize minimumSize = minimumActionSizes[actionIndex++];
        if (!approximately_equal(NSWidth(action.frame), minimumSize.width)
            || !approximately_equal(NSHeight(action.frame), minimumSize.height)) {
            error = "a header action resized when the window widened";
            restore();
            return NO;
        }
    }
    restore();
    return YES;
}

- (BOOL)validateLayout:(std::string&)error {
    error.clear();
    [self.window.contentView layoutSubtreeIfNeeded];
    NSView* ambiguous = self.window.contentView.hasAmbiguousLayout
        ? self.window.contentView
        : first_ambiguous_visible_view(self.window.contentView);
    if (ambiguous != nil) {
        error = "main window visible hierarchy has an ambiguous Auto Layout result: "
            + utf8_string(NSStringFromClass(ambiguous.class));
        if (ambiguous.identifier.length > 0) {
            error += " [" + utf8_string(ambiguous.identifier) + "]";
        }
        if (ambiguous.accessibilityLabel.length > 0) {
            error += " (" + utf8_string(ambiguous.accessibilityLabel) + ")";
        }
        error += " frame=" + utf8_string(NSStringFromRect(ambiguous.frame));
        if (ambiguous.superview != nil) {
            error += " parent=" + utf8_string(NSStringFromClass(ambiguous.superview.class));
        }
        if ([ambiguous isKindOfClass:NSStackView.class]) {
            error += " arranged=" + std::to_string(
                static_cast<NSStackView*>(ambiguous).arrangedSubviews.count);
        }
        return NO;
    }
    const NSSize frame_size = self.window.frame.size;
    if (frame_size.width + 0.5 < self.window.minSize.width
        || frame_size.height + 0.5 < self.window.minSize.height) {
        error = "main window is smaller than its declared minimum size";
        return NO;
    }
    if (self.window.backingScaleFactor <= 0.0) {
        error = "main window has no valid backing scale";
        return NO;
    }
    if (![self validateResponsiveLayout:error]) {
        return NO;
    }
    if ([_profilesView isKindOfClass:NSSplitView.class]) {
        error = "Profiles layout still exposes a movable split divider";
        return NO;
    }
    const BOOL profileFrameRounded = _profileListFrame.boxType == NSBoxCustom
        && _profileListFrame.borderWidth >= 0.5
        && _profileListFrame.cornerRadius >= 7.5
        && _profileListFrame.layer.cornerRadius >= 7.5
        && _profileListFrame.layer.masksToBounds;
    const BOOL rulesFrameRounded = _rulesEditorFrame.boxType == NSBoxCustom
        && _rulesEditorFrame.borderWidth >= 0.5
        && _rulesEditorFrame.cornerRadius >= 7.5
        && _rulesEditorFrame.layer.cornerRadius >= 7.5
        && _rulesEditorFrame.layer.masksToBounds;
    if (!profileFrameRounded || !rulesFrameRounded) {
        error = "an editor frame is missing rounded clipping";
        return NO;
    }
    NSView* badgeContent = _serviceStatusBadge.contentView;
    const CGFloat statusCenterXDelta =
        NSMidX(_serviceStatus.frame) - NSMidX(badgeContent.bounds);
    const CGFloat statusCenterYDelta =
        NSMidY(_serviceStatus.frame) - NSMidY(badgeContent.bounds);
    if (statusCenterXDelta < -1.0 || statusCenterXDelta > 1.0
        || statusCenterYDelta < -1.0 || statusCenterYDelta > 1.0
        || _serviceStatus.alignment != NSTextAlignmentCenter
        || _serviceStatusBadge.cornerRadius < 11.5
        || NSWidth(_serviceStatusBadge.bounds) < 91.5
        || NSHeight(_serviceStatusBadge.bounds) < 23.5) {
        error = "service status text is not centered in its capsule";
        return NO;
    }
    if (_currentView == CCSMainWindowViewProfiles) {
        const NSRect documentBounds = _profileDetailsDocument.bounds;
        if (NSWidth(documentBounds) < 1.0 || NSHeight(documentBounds) < 1.0) {
            error = "Profile details document has an empty frame";
            return NO;
        }
        NSRect previous = NSZeroRect;
        BOOL hasPrevious = NO;
        for (NSView* row in _profileDetailRows) {
            const NSRect frame = [row convertRect:row.bounds toView:_profileDetailsDocument];
            if (NSWidth(frame) < 1.0 || NSHeight(frame) < 1.0) {
                error = "a Profile details row has an empty frame";
                return NO;
            }
            if (!NSContainsRect(NSInsetRect(documentBounds, -0.5, -0.5), frame)) {
                error = "a Profile details row extends outside its document";
                return NO;
            }
            if (hasPrevious && NSMinY(frame) + 0.5 < NSMaxY(previous)) {
                error = "Profile details rows overlap or are out of order";
                return NO;
            }
            previous = frame;
            hasPrevious = YES;
        }
    }
    NSMutableArray<NSView*>* required_controls = [NSMutableArray arrayWithArray:@[
        _brandLabel, _serviceStatus, _listenerStatus, _startButton, _stopButton, _reloadButton,
        _lightweightCheckbox, _profilesNavigationButton, _rulesNavigationButton,
        _settingsNavigationButton, _profileTable, _newProfileField, _addButton, _removeButton,
        _renameField, _renameButton, _enabledCheckbox, _readinessValue, _rulesValue,
        _profileDetail, _updateProfileFieldsButton, _rulesProfilePopup, _rulesTextView,
        _rulesStatus, _formatRulesButton, _updateRulesButton, _updateSettingsButton,
        _commandStatus, _reloadDraftButton, _applyButton, _discardButton,
    ]];
    for (NSDictionary* entry in _profileFieldControls) {
        [required_controls addObject:entry[@"input"]];
    }
    for (NSDictionary* entry in _settingsFieldControls) {
        [required_controls addObject:entry[@"input"]];
    }
    for (NSView* control in required_controls) {
        if (control.accessibilityLabel.length == 0) {
            error = "a main window control is missing its accessibility label";
            return NO;
        }
    }
    return YES;
}

- (BOOL)validateKeyboard:(std::string&)error {
    error.clear();
    if (self.window.defaultButtonCell == nil) {
        error = "main window has no default button";
        return NO;
    }
    NSMutableArray<NSView*>* expected = [NSMutableArray arrayWithArray:@[
        _startButton, _stopButton, _reloadButton, _lightweightCheckbox,
        _profilesNavigationButton, _rulesNavigationButton, _settingsNavigationButton,
    ]];
    if (_currentView == CCSMainWindowViewProfiles) {
        [expected addObjectsFromArray:@[
            _profileTable, _newProfileField, _addButton, _removeButton,
            _renameField, _renameButton, _enabledCheckbox,
        ]];
        for (NSDictionary* entry in _profileFieldControls) {
            [expected addObject:entry[@"input"]];
        }
        [expected addObject:_updateProfileFieldsButton];
    } else if (_currentView == CCSMainWindowViewRules) {
        [expected addObjectsFromArray:@[
            _rulesProfilePopup, _rulesTextView, _formatRulesButton, _updateRulesButton,
        ]];
    } else {
        for (NSDictionary* entry in _settingsFieldControls) {
            [expected addObject:entry[@"input"]];
        }
        [expected addObject:_updateSettingsButton];
    }
    [expected addObjectsFromArray:@[_reloadDraftButton, _discardButton, _applyButton]];
    NSSet<NSView*>* required = [NSSet setWithArray:expected];
    NSMutableSet<NSView*>* visited = [NSMutableSet set];
    NSView* current = expected.firstObject;
    for (NSInteger step = 0; current != nil && step < 128; ++step) {
        [visited addObject:current];
        current = current.nextKeyView;
        if (current == expected.firstObject) {
            break;
        }
    }
    if (current != expected.firstObject || ![required isSubsetOfSet:visited]) {
        error = "main window Tab key loop does not reach every interactive editing control";
        return NO;
    }
    return YES;
}

- (BOOL)validateRetina:(std::string&)error {
    error.clear();
    if (self.window.backingScaleFactor < 2.0) {
        error = "main window is not on a Retina backing scale";
        return NO;
    }
    return YES;
}

- (BOOL)validateProfileRuleSummary:(std::string&)error {
    error.clear();
    if (_owner == nullptr || _owner->selected_profile() == nullptr) {
        error = "Profile Rule summary probe requires a selected Profile";
        return NO;
    }
    const auto* profile = _owner->selected_profile();
    NSString* expected = [NSString stringWithFormat:@"%zu enabled / %zu total",
        profile->enabled_rule_count,
        profile->rule_count];
    if (![_rulesValue.stringValue isEqualToString:expected]) {
        error = "Profile Rule summary does not match the shared snapshot";
        return NO;
    }
    return YES;
}

- (void)showProfiles:(id)sender {
    (void)sender;
    [self showView:CCSMainWindowViewProfiles];
}

- (void)showRules:(id)sender {
    (void)sender;
    [self showView:CCSMainWindowViewRules];
}

- (void)showSettings:(id)sender {
    (void)sender;
    [self showView:CCSMainWindowViewSettings];
}

- (void)selectRulesProfile:(id)sender {
    (void)sender;
    if (_updating || _owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    const NSInteger index = _rulesProfilePopup.indexOfSelectedItem;
    if (state == nullptr || index < 0
        || static_cast<std::size_t>(index) >= state->profiles.size()) {
        return;
    }
    const auto& profile = state->profiles[static_cast<std::size_t>(index)];
    if (state->rules_editor && state->rules_editor->profile_key == profile.key) {
        return;
    }
    if (![self confirmDiscardProfileEditors]) {
        _updating = YES;
        [self render];
        _updating = NO;
        return;
    }
    ccs::MainWindowCommandRequest request{
        ccs::MainWindowCommand::SelectProfile, profile.id};
    request.profile_key = profile.key;
    _owner->submit(std::move(request));
}

- (void)updateProfileFields:(id)sender {
    (void)sender;
    if (_owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state == nullptr || !state->profile_editor || !state->selected_profile_key) {
        return;
    }
    if (_hasProfileLocalKey && _profileLocalKey != state->profile_editor->key) {
        [self setLocalStatus:@"The selected Profile changed; discard and re-enter local edits." error:YES];
        return;
    }
    std::vector<ccs::ConfigurationFieldEdit> edits;
    std::string error;
    if (![self collectFieldEdits:_profileFieldControls
        states:state->profile_editor->fields edits:edits error:error]) {
        [self setLocalStatus:ns_string(error) error:YES];
        return;
    }
    if (edits.empty()) {
        _profileLocalDirty = NO;
        _hasProfileLocalKey = NO;
        [self setLocalStatus:@"Profile fields are up to date." error:NO];
        [self render];
        return;
    }
    ccs::MainWindowCommandRequest request;
    request.command = ccs::MainWindowCommand::UpdateProfileFields;
    request.profile_id = state->profile_editor->profile_id;
    request.profile_key = state->profile_editor->key;
    request.field_edits = std::move(edits);
    _profileUpdatePreviousSequence = state->last_command
        ? state->last_command->sequence : 0;
    if (_owner->submit(std::move(request))) {
        _profileUpdateAwaiting = YES;
    }
}

- (void)updateSettings:(id)sender {
    (void)sender;
    if (_owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state == nullptr) {
        return;
    }
    std::vector<ccs::ConfigurationFieldEdit> edits;
    std::string error;
    if (![self collectFieldEdits:_settingsFieldControls
        states:state->application_fields edits:edits error:error]) {
        [self setLocalStatus:ns_string(error) error:YES];
        return;
    }
    if (edits.empty()) {
        _settingsLocalDirty = NO;
        [self setLocalStatus:@"Settings are up to date." error:NO];
        [self render];
        return;
    }
    ccs::MainWindowCommandRequest request;
    request.command = ccs::MainWindowCommand::UpdateApplicationFields;
    request.field_edits = std::move(edits);
    _settingsUpdatePreviousSequence = state->last_command
        ? state->last_command->sequence : 0;
    if (_owner->submit(std::move(request))) {
        _settingsUpdateAwaiting = YES;
    }
}

- (void)submitRulesUpdate:(ccs::MainWindowCommand)command {
    if (_owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state == nullptr || !state->rules_editor) {
        [self setLocalStatus:@"Select a Profile before updating Rules." error:YES];
        return;
    }
    if (_hasRulesLocalKey && _rulesLocalKey != state->rules_editor->profile_key) {
        [self setLocalStatus:@"The selected Profile changed; discard and re-enter local Rules." error:YES];
        return;
    }
    ccs::MainWindowCommandRequest request;
    request.command = command;
    request.profile_id = state->rules_editor->profile_id;
    request.profile_key = state->rules_editor->profile_key;
    request.text = canonical_newlines(utf8_string(_rulesTextView.string));
    _rulesUpdatePreviousSequence = state->last_command
        ? state->last_command->sequence : 0;
    if (_owner->submit(std::move(request))) {
        _rulesUpdateAwaiting = YES;
    }
}

- (void)formatRules:(id)sender {
    (void)sender;
    [self submitRulesUpdate:ccs::MainWindowCommand::FormatRulesText];
}

- (void)updateRules:(id)sender {
    (void)sender;
    [self submitRulesUpdate:ccs::MainWindowCommand::ReplaceRulesText];
}

- (void)startService:(id)sender {
    (void)sender;
    if (_owner != nullptr) _owner->submit({ccs::MainWindowCommand::StartService});
}

- (void)stopService:(id)sender {
    (void)sender;
    if (_owner != nullptr) _owner->submit({ccs::MainWindowCommand::StopService});
}

- (void)reloadService:(id)sender {
    (void)sender;
    if (_owner != nullptr) _owner->submit({ccs::MainWindowCommand::ReloadService});
}

- (void)toggleLightweight:(id)sender {
    (void)sender;
    if (_owner != nullptr && !_updating) {
        _owner->submit({
            ccs::MainWindowCommand::SetLightweightMode,
            {},
            {},
            _lightweightCheckbox.state == NSControlStateValueOn,
        });
    }
}

- (void)addProfile:(id)sender {
    (void)sender;
    if (_owner == nullptr) return;
    const auto profile_id = utf8_string(_newProfileField.stringValue);
    if (profile_id.empty()) {
        [self setLocalStatus:@"Enter a Profile ID before adding it." error:YES];
        return;
    }
    _owner->submit({ccs::MainWindowCommand::CreateProfile, profile_id});
    _newProfileField.stringValue = @"";
}

- (void)removeProfile:(id)sender {
    (void)sender;
    if (_owner == nullptr || _owner->selected_profile() == nullptr) return;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Remove Profile?";
    alert.informativeText = @"Remove the selected Profile from this draft?";
    [alert addButtonWithTitle:@"Remove"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] == NSAlertFirstButtonReturn && _owner != nullptr) {
        const auto state = _owner->state();
        if (state && state->selected_profile_id) {
            ccs::MainWindowCommandRequest request{
                ccs::MainWindowCommand::RemoveProfile, *state->selected_profile_id};
            request.profile_key = state->selected_profile_key;
            _owner->submit(std::move(request));
        }
    }
}

- (void)renameProfile:(id)sender {
    (void)sender;
    if (_owner == nullptr) return;
    const auto state = _owner->state();
    const auto replacement = utf8_string(_renameField.stringValue);
    if (!state || !state->selected_profile_id) return;
    if (replacement.empty()) {
        [self setLocalStatus:@"Profile ID cannot be empty." error:YES];
        return;
    }
    ccs::MainWindowCommandRequest request{
        ccs::MainWindowCommand::RenameProfile,
        *state->selected_profile_id,
        replacement};
    request.profile_key = state->selected_profile_key;
    _owner->submit(std::move(request));
}

- (void)toggleProfileEnabled:(id)sender {
    (void)sender;
    if (_owner == nullptr || _updating) return;
    const auto state = _owner->state();
    if (state && state->selected_profile_id) {
        ccs::MainWindowCommandRequest request{
            ccs::MainWindowCommand::SetProfileEnabled,
            *state->selected_profile_id,
            {},
            _enabledCheckbox.state == NSControlStateValueOn};
        request.profile_key = state->selected_profile_key;
        _owner->submit(std::move(request));
    }
}

- (void)applyDraft:(id)sender {
    (void)sender;
    if (_owner == nullptr) return;
    if (_profileLocalDirty || _rulesLocalDirty || _settingsLocalDirty
        || _profileUpdateAwaiting || _rulesUpdateAwaiting || _settingsUpdateAwaiting) {
        [self setLocalStatus:@"Update the current editor before applying changes." error:YES];
        return;
    }
    _owner->submit({ccs::MainWindowCommand::ApplyDraft});
}

- (void)reloadDraft:(id)sender {
    (void)sender;
    if (_owner == nullptr) return;
    const auto state = _owner->state();
    if (state == nullptr) {
        return;
    }
    const bool has_local = [self hasLocalEdits];
    if (!state->draft.dirty() && !has_local) {
        _owner->submit({ccs::MainWindowCommand::ReloadDraft});
        return;
    }
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Reload configuration?";
    alert.informativeText = @"Discard local and draft changes and reload the configuration from disk?";
    [alert addButtonWithTitle:@"Discard and Reload"];
    [alert addButtonWithTitle:@"Cancel"];
    if ([alert runModal] == NSAlertFirstButtonReturn && _owner != nullptr) {
        [self discardLocalEdits];
        _owner->submit({
            ccs::MainWindowCommand::ReloadDraft,
            {},
            {},
            false,
            ccs::UnsavedChangesDecision::Discard,
        });
    }
}

- (void)discardDraft:(id)sender {
    (void)sender;
    if (_owner != nullptr) {
        [self discardLocalEdits];
        _owner->submit({ccs::MainWindowCommand::DiscardDraft});
    }
}

@end

namespace ccs {

MacMainWindow::Impl::Impl(
    MainWindowViewModel& view_model,
    LifecycleHandler lifecycle_handler)
    : view_model_(view_model)
    , lifecycle_handler_(std::move(lifecycle_handler)) {}

MacMainWindow::Impl::~Impl() {
    destroy();
}

bool MacMainWindow::Impl::show(MainWindowStateSnapshot state, std::string& error) {
    error.clear();
    state_ = std::move(state);
    const bool created = controller_ == nil;
    if (created) {
        controller_ = [[CCSMainWindowController alloc] initWithOwner:this];
        if (controller_ == nil || controller_.window == nil) {
            controller_ = nil;
            error = "failed to create the AppKit main window";
            return false;
        }
        notify_lifecycle("created");
    }
    [controller_ render];
    [controller_ prepareForDisplay:NO];
    [controller_ showWindow:nil];
    [controller_ prepareForDisplay:created];
    if (controller_.window.miniaturized) {
        [controller_.window deminiaturize:nil];
    }
    [NSApp activateIgnoringOtherApps:YES];
    [controller_.window makeKeyAndOrderFront:nil];
    [controller_ configureKeyLoop];
    notify_lifecycle("shown");
    return true;
}

void MacMainWindow::Impl::update(MainWindowStateSnapshot state) {
    state_ = std::move(state);
    if (controller_ != nil) {
        [controller_ render];
    }
    finish_pending_close();
}

bool MacMainWindow::Impl::prepare_for_application_exit(
    std::function<void()> continuation) {
    state_ = view_model_.snapshot();
    if (!state_) {
        return true;
    }
    std::string error;
    if (state_->command_pending || state_->draft.busy()) {
        if (!show(state_, error)) {
            return false;
        }
        [controller_ setLocalStatus:@"Wait for the current command to finish." error:YES];
        return false;
    }
    if (controller_ != nil && [controller_ hasLocalEdits]) {
        if (!show(state_, error) || ![controller_ confirmDiscardLocalEdits]) {
            return false;
        }
        state_ = view_model_.snapshot();
    }
    if (!state_->draft.dirty()) {
        return true;
    }
    if (!show(state_, error)) {
        return false;
    }
    return request_close(CloseTarget::ExitApplication, std::move(continuation));
}

void MacMainWindow::Impl::destroy() {
    pending_close_ = {};
    if (controller_ == nil) {
        return;
    }
    [controller_ invalidateOwner];
    NSWindow* window = controller_.window;
    [controller_ setWindow:nil];
    [window orderOut:nil];
    [window close];
    controller_ = nil;
    notify_lifecycle("destroyed");
}

bool MacMainWindow::Impl::exists() const noexcept {
    return controller_ != nil && controller_.window != nil;
}

bool MacMainWindow::Impl::visible() const noexcept {
    return exists() && controller_.window.visible;
}

const ProfileListItem* MacMainWindow::Impl::selected_profile() const noexcept {
    if (!state_) {
        return nullptr;
    }
    if (state_->selected_profile_key) {
        if (const auto* selected = find_profile_list_item(*state_, *state_->selected_profile_key)) {
            return selected;
        }
    }
    return state_->selected_profile_id
        ? find_profile_list_item(*state_, *state_->selected_profile_id)
        : nullptr;
}

bool MacMainWindow::Impl::submit(MainWindowCommandRequest request) {
    if (controller_ != nil) {
        [controller_ setLocalStatus:@"" error:NO];
    }
    const bool accepted = view_model_.submit(std::move(request));
    if (!accepted && controller_ != nil) {
        [controller_ setLocalStatus:@"Another command is already running." error:YES];
    }
    return accepted;
}

void MacMainWindow::Impl::request_window_close(
    std::optional<UnsavedChangesDecision> decision) {
    state_ = view_model_.snapshot();
    if (!state_) {
        perform_close(CloseTarget::Destroy);
        return;
    }
    if (state_->command_pending || state_->draft.busy()) {
        if (controller_ != nil) {
            [controller_ setLocalStatus:@"Wait for the current command to finish." error:YES];
        }
        return;
    }
    if (controller_ != nil && [controller_ hasLocalEdits]) {
        if (decision && *decision == UnsavedChangesDecision::Cancel) {
            return;
        }
        if (decision && *decision == UnsavedChangesDecision::Apply) {
            [controller_ setLocalStatus:@"Update the current editor before applying changes." error:YES];
            return;
        }
        if (decision && *decision == UnsavedChangesDecision::Discard) {
            [controller_ discardLocalEdits];
        } else if (![controller_ confirmDiscardLocalEdits]) {
            return;
        }
        state_ = view_model_.snapshot();
    }
    const auto action = resolve_main_window_close(
        state_->draft, state_->lightweight_mode, std::nullopt);
    if (action == MainWindowCloseAction::Hide) {
        perform_close(CloseTarget::Hide);
    } else if (action == MainWindowCloseAction::Destroy) {
        perform_close(CloseTarget::Destroy);
    } else if (action == MainWindowCloseAction::KeepOpen) {
        if (controller_ != nil) {
            [controller_ setLocalStatus:@"Wait for the current command to finish." error:YES];
        }
    } else {
        (void)request_close(
            state_->lightweight_mode ? CloseTarget::Destroy : CloseTarget::Hide,
            {},
            decision);
    }
}

bool MacMainWindow::Impl::request_close(
    CloseTarget target,
    std::function<void()> continuation,
    std::optional<UnsavedChangesDecision> decision) {
    if (!state_) {
        return true;
    }
    if (state_->draft.busy() || state_->command_pending) {
        if (controller_ != nil) {
            [controller_ setLocalStatus:@"Wait for the current command to finish." error:YES];
        }
        return false;
    }
    if (controller_ != nil && [controller_ hasLocalEdits]
        && ![controller_ confirmDiscardLocalEdits]) {
        return false;
    }
    if (!state_->draft.dirty()) {
        return true;
    }
    const auto selected_decision = decision ? decision : prompt_unsaved_changes();
    if (!selected_decision || *selected_decision == UnsavedChangesDecision::Cancel) {
        return false;
    }
    const auto command = *selected_decision == UnsavedChangesDecision::Apply
        ? MainWindowCommand::ApplyDraft
        : MainWindowCommand::DiscardDraft;
    const auto previous_sequence = state_->last_command
        ? state_->last_command->sequence
        : 0;
    if (!view_model_.submit({command})) {
        if (controller_ != nil) {
            [controller_ setLocalStatus:@"Another command is already running." error:YES];
        }
        return false;
    }
    pending_close_ = PendingClose{
        target,
        command,
        previous_sequence,
        std::move(continuation),
    };
    return false;
}

void MacMainWindow::Impl::finish_pending_close() {
    if (pending_close_.target == CloseTarget::None
        || !state_
        || state_->command_pending
        || !state_->last_command
        || state_->last_command->sequence <= pending_close_.previous_sequence
        || state_->last_command->command != pending_close_.command) {
        return;
    }
    auto pending = std::move(pending_close_);
    pending_close_ = {};
    const bool completed = state_->last_command->succeeded()
        || (pending.command == MainWindowCommand::ApplyDraft
            && state_->last_command->configuration_saved());
    if (completed) {
        perform_close(pending.target, std::move(pending.continuation));
    }
}

void MacMainWindow::Impl::perform_close(
    CloseTarget target,
    std::function<void()> continuation) {
    if (target == CloseTarget::Hide && controller_ != nil) {
        [controller_.window orderOut:nil];
        notify_lifecycle("hidden");
    } else if (target == CloseTarget::Destroy) {
        destroy();
    } else if (target == CloseTarget::ExitApplication && continuation) {
        continuation();
    }
}

std::optional<UnsavedChangesDecision> MacMainWindow::Impl::prompt_unsaved_changes() {
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Unsaved Profile changes";
    alert.informativeText = @"Apply changes before closing?";
    [alert addButtonWithTitle:@"Apply"];
    [alert addButtonWithTitle:@"Discard"];
    [alert addButtonWithTitle:@"Cancel"];
    const auto choice = [alert runModal];
    if (choice == NSAlertFirstButtonReturn) {
        return UnsavedChangesDecision::Apply;
    }
    if (choice == NSAlertSecondButtonReturn) {
        return UnsavedChangesDecision::Discard;
    }
    return UnsavedChangesDecision::Cancel;
}

void MacMainWindow::Impl::notify_lifecycle(std::string_view event) const {
    if (lifecycle_handler_) {
        lifecycle_handler_(event);
    }
}

bool MacMainWindow::Impl::run_test_command(std::string_view command, std::string& error) {
    error.clear();
    const auto argument = [&](std::string_view prefix) -> std::optional<std::string> {
        if (!command.starts_with(prefix)) {
            return std::nullopt;
        }
        return std::string(command.substr(prefix.size()));
    };
    if (command == "probe") {
        return controller_ == nil || [controller_ validateLayout:error];
    }
    if (command == "probe:keyboard") {
        return controller_ != nil && [controller_ validateKeyboard:error];
    }
    if (command == "probe:retina") {
        return controller_ != nil && [controller_ validateRetina:error];
    }
    if (command == "probe:profile-rule-summary") {
        return controller_ != nil && [controller_ validateProfileRuleSummary:error];
    }
    if (command == "probe:responsive-layout") {
        return controller_ != nil && [controller_ validateResponsiveBehavior:error];
    }
    if (command == "view:profiles" || command == "view:rules" || command == "view:settings") {
        if (controller_ == nil) {
            error = "view test command requires an existing main window";
            return false;
        }
        [controller_ showView:(command == "view:profiles"
                ? CCSMainWindowViewProfiles
                : (command == "view:rules"
                    ? CCSMainWindowViewRules : CCSMainWindowViewSettings))];
        return [controller_ validateLayout:error];
    }
    if (command == "probe:views") {
        if (controller_ == nil) {
            error = "view probe requires an existing main window";
            return false;
        }
        const auto original = [controller_ currentView];
        for (const auto view : {
                 CCSMainWindowViewProfiles,
                 CCSMainWindowViewRules,
                 CCSMainWindowViewSettings}) {
            [controller_ showView:view];
            if (![controller_ validateLayout:error]) {
                [controller_ showView:original];
                return false;
            }
        }
        [controller_ showView:original];
        return true;
    }
    if (command == "resize:min") {
        if (controller_ == nil) {
            error = "resize:min requires an existing main window";
            return false;
        }
        NSRect frame = controller_.window.frame;
        frame.size = controller_.window.minSize;
        [controller_.window setFrame:frame display:YES];
        return [controller_ validateLayout:error];
    }
    if (command == "appearance:light" || command == "appearance:dark") {
        if (controller_ == nil) {
            error = "appearance test requires an existing main window";
            return false;
        }
        controller_.window.appearance = [NSAppearance appearanceNamed:(
            command == "appearance:light"
                ? NSAppearanceNameAqua
                : NSAppearanceNameDarkAqua)];
        return [controller_ validateLayout:error];
    }
    if (command == "quit:discard") {
        state_ = view_model_.snapshot();
        if (!state_ || !state_->draft.dirty()) {
            error = "quit:discard requires a dirty draft";
            return false;
        }
        (void)request_close(
            CloseTarget::ExitApplication,
            []() { [NSApp terminate:nil]; },
            UnsavedChangesDecision::Discard);
    } else if (const auto value = argument("cycle:")) {
        int cycles = 0;
        const auto parsed = std::from_chars(
            value->data(), value->data() + value->size(), cycles);
        if (parsed.ec != std::errc{}
            || parsed.ptr != value->data() + value->size()
            || cycles < 1
            || cycles > 200) {
            error = "cycle test command requires a count from 1 through 200";
            return false;
        }
        const auto current = view_model_.snapshot();
        if (!current || !current->lightweight_mode || current->draft.dirty()) {
            error = "window cycles require a clean lightweight-mode draft";
            return false;
        }
        for (int cycle = 0; cycle < cycles; ++cycle) {
            @autoreleasepool {
                if (!show(view_model_.snapshot(), error)) {
                    return false;
                }
                request_window_close();
                if (exists()) {
                    error = "lightweight window remained cached after a test close";
                    return false;
                }
            }
        }
    } else if (command == "close") {
        request_window_close();
    } else if (command == "close:apply") {
        request_window_close(UnsavedChangesDecision::Apply);
    } else if (command == "close:discard") {
        request_window_close(UnsavedChangesDecision::Discard);
    } else if (command == "close:cancel") {
        request_window_close(UnsavedChangesDecision::Cancel);
    } else if (command == "apply") {
        submit({MainWindowCommand::ApplyDraft});
    } else if (command == "discard") {
        submit({MainWindowCommand::DiscardDraft});
    } else if (command == "reload-draft") {
        submit({MainWindowCommand::ReloadDraft});
    } else if (command == "reload-draft:discard") {
        submit({
            MainWindowCommand::ReloadDraft,
            {},
            {},
            false,
            UnsavedChangesDecision::Discard,
        });
    } else if (command == "start") {
        submit({MainWindowCommand::StartService});
    } else if (command == "stop") {
        submit({MainWindowCommand::StopService});
    } else if (command == "reload") {
        submit({MainWindowCommand::ReloadService});
    } else if (const auto value = argument("lightweight:")) {
        submit({MainWindowCommand::SetLightweightMode, {}, {}, *value == "1"});
    } else if (const auto value = argument("select:")) {
        submit({MainWindowCommand::SelectProfile, *value});
    } else if (const auto value = argument("create:")) {
        submit({MainWindowCommand::CreateProfile, *value});
    } else if (const auto value = argument("remove:")) {
        submit({MainWindowCommand::RemoveProfile, *value});
    } else if (const auto value = argument("enable:")) {
        submit({MainWindowCommand::SetProfileEnabled, *value, {}, true});
    } else if (const auto value = argument("disable:")) {
        submit({MainWindowCommand::SetProfileEnabled, *value, {}, false});
    } else if (const auto value = argument("rename:")) {
        const auto separator = value->find(':');
        if (separator == std::string::npos) {
            error = "rename test command requires old and new Profile IDs";
            return false;
        }
        submit({
            MainWindowCommand::RenameProfile,
            value->substr(0, separator),
            value->substr(separator + 1),
        });
    } else {
        error = "unknown main window test command";
        return false;
    }
    return true;
}

MacMainWindow::MacMainWindow(
    MainWindowViewModel& view_model,
    LifecycleHandler lifecycle_handler)
    : impl_(std::make_unique<Impl>(view_model, std::move(lifecycle_handler))) {}

MacMainWindow::~MacMainWindow() = default;

bool MacMainWindow::show(MainWindowStateSnapshot state, std::string& error) {
    return impl_->show(std::move(state), error);
}

void MacMainWindow::update(MainWindowStateSnapshot state) {
    impl_->update(std::move(state));
}

bool MacMainWindow::prepare_for_application_exit(std::function<void()> continuation) {
    return impl_->prepare_for_application_exit(std::move(continuation));
}

void MacMainWindow::destroy() {
    impl_->destroy();
}

bool MacMainWindow::exists() const noexcept {
    return impl_->exists();
}

bool MacMainWindow::visible() const noexcept {
    return impl_->visible();
}

std::int64_t MacMainWindow::live_window_count() noexcept {
    return ::live_window_count;
}

std::int64_t MacMainWindow::live_controller_count() noexcept {
    return ::live_controller_count;
}

bool MacMainWindow::run_test_command(std::string_view command, std::string& error) {
    return impl_->run_test_command(command, error);
}

} // namespace ccs

#endif
