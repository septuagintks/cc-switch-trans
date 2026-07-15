#include "hosts/macos/main_window.hpp"

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <charconv>
#include <optional>
#include <utility>

namespace {

std::int64_t live_window_count = 0;
std::int64_t live_controller_count = 0;

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

} // namespace

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
    void submit(MainWindowCommandRequest request);
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
    NSTableViewDelegate> {
    ccs::MacMainWindow::Impl* _owner;
    NSTextField* _serviceStatus;
    NSTextField* _listenerStatus;
    NSButton* _startButton;
    NSButton* _stopButton;
    NSButton* _reloadButton;
    NSButton* _lightweightCheckbox;
    NSTableView* _profileTable;
    NSTextField* _newProfileField;
    NSButton* _addButton;
    NSButton* _removeButton;
    NSTextField* _renameField;
    NSButton* _renameButton;
    NSButton* _enabledCheckbox;
    NSTextField* _protocolValue;
    NSTextField* _readinessValue;
    NSTextField* _profileDetail;
    NSTextField* _commandStatus;
    NSButton* _applyButton;
    NSButton* _discardButton;
    BOOL _updating;
    BOOL _resourceCounted;
}

- (instancetype)initWithOwner:(ccs::MacMainWindow::Impl*)owner;
- (void)invalidateOwner;
- (void)render;
- (void)configureKeyLoop;
- (void)setLocalStatus:(NSString*)message error:(BOOL)error;
- (BOOL)validateLayout:(std::string&)error;
- (BOOL)validateKeyboard:(std::string&)error;
- (BOOL)validateRetina:(std::string&)error;

@end

@implementation CCSMainWindowController

- (instancetype)initWithOwner:(ccs::MacMainWindow::Impl*)owner {
    NSWindow* window = [[CCSMainWindowResource alloc]
        initWithContentRect:NSMakeRect(0.0, 0.0, 900.0, 590.0)
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
    window.minSize = NSMakeSize(760.0, 500.0);
    window.restorable = NO;
    window.tabbingMode = NSWindowTabbingModeDisallowed;
    window.excludedFromWindowsMenu = YES;
    window.autorecalculatesKeyViewLoop = NO;
    window.delegate = self;
    [window center];

    _serviceStatus = label(@"Service: stopped", @"Service status");
    _serviceStatus.font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:NSFontWeightSemibold];
    _listenerStatus = label(@"Listener inactive", @"Listener address");
    _startButton = push_button(@"Start", self, @selector(startService:), @"Start service");
    _stopButton = push_button(@"Stop", self, @selector(stopService:), @"Stop service");
    _reloadButton = push_button(@"Reload", self, @selector(reloadService:), @"Reload service");
    _lightweightCheckbox = [NSButton checkboxWithTitle:@"Lightweight Mode"
        target:self
        action:@selector(toggleLightweight:)];
    _lightweightCheckbox.accessibilityLabel = @"Lightweight Mode";

    NSStackView* serviceText = [NSStackView stackViewWithViews:@[_serviceStatus, _listenerStatus]];
    serviceText.orientation = NSUserInterfaceLayoutOrientationVertical;
    serviceText.alignment = NSLayoutAttributeLeading;
    serviceText.spacing = 3.0;
    NSStackView* serviceActions = [NSStackView stackViewWithViews:@[
        _startButton, _stopButton, _reloadButton, _lightweightCheckbox]];
    serviceActions.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    serviceActions.alignment = NSLayoutAttributeCenterY;
    serviceActions.spacing = 8.0;
    NSStackView* serviceRow = [NSStackView stackViewWithViews:@[serviceText, serviceActions]];
    serviceRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    serviceRow.alignment = NSLayoutAttributeCenterY;
    serviceRow.distribution = NSStackViewDistributionFill;
    [serviceText setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [serviceActions setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];

    _profileTable = [[NSTableView alloc] initWithFrame:NSZeroRect];
    _profileTable.dataSource = self;
    _profileTable.delegate = self;
    _profileTable.allowsMultipleSelection = NO;
    _profileTable.allowsEmptySelection = YES;
    _profileTable.usesAlternatingRowBackgroundColors = YES;
    _profileTable.accessibilityLabel = @"Profiles";
    NSTableColumn* profileColumn = [[NSTableColumn alloc] initWithIdentifier:@"profile"];
    profileColumn.title = @"Profile";
    profileColumn.minWidth = 110.0;
    NSTableColumn* enabledColumn = [[NSTableColumn alloc] initWithIdentifier:@"enabled"];
    enabledColumn.title = @"Enabled";
    enabledColumn.width = 68.0;
    enabledColumn.minWidth = 62.0;
    NSTableColumn* readinessColumn = [[NSTableColumn alloc] initWithIdentifier:@"readiness"];
    readinessColumn.title = @"Readiness";
    readinessColumn.width = 90.0;
    readinessColumn.minWidth = 80.0;
    [_profileTable addTableColumn:profileColumn];
    [_profileTable addTableColumn:enabledColumn];
    [_profileTable addTableColumn:readinessColumn];
    NSScrollView* tableScroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    tableScroll.documentView = _profileTable;
    tableScroll.hasVerticalScroller = YES;
    tableScroll.hasHorizontalScroller = YES;
    tableScroll.borderType = NSBezelBorder;

    _newProfileField = [[NSTextField alloc] initWithFrame:NSZeroRect];
    _newProfileField.placeholderString = @"New Profile ID";
    _newProfileField.accessibilityLabel = @"New Profile ID";
    _addButton = push_button(@"Add", self, @selector(addProfile:), @"Add Profile");
    _removeButton = push_button(@"Remove", self, @selector(removeProfile:), @"Remove selected Profile");
    NSStackView* addRow = [NSStackView stackViewWithViews:@[
        _newProfileField, _addButton, _removeButton]];
    addRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    addRow.spacing = 8.0;
    [_newProfileField setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* profiles = [NSStackView stackViewWithViews:@[
        label(@"Profiles", @"Profiles section"), tableScroll, addRow]];
    profiles.orientation = NSUserInterfaceLayoutOrientationVertical;
    profiles.alignment = NSLayoutAttributeLeading;
    profiles.spacing = 8.0;
    [tableScroll.widthAnchor constraintGreaterThanOrEqualToConstant:275.0].active = YES;
    [tableScroll.heightAnchor constraintGreaterThanOrEqualToConstant:270.0].active = YES;
    [tableScroll.widthAnchor constraintEqualToAnchor:profiles.widthAnchor].active = YES;
    [addRow.widthAnchor constraintEqualToAnchor:profiles.widthAnchor].active = YES;

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
    _protocolValue = label(@"No Profile selected", @"Profile protocol");
    _readinessValue = label(@"", @"Profile readiness");
    _profileDetail = label(@"", @"Profile validation detail");
    _profileDetail.lineBreakMode = NSLineBreakByWordWrapping;
    _profileDetail.maximumNumberOfLines = 0;
    [_profileDetail setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationVertical];
    NSGridView* detailsGrid = [NSGridView gridViewWithViews:@[
        @[label(@"Protocol", @"Protocol label"), _protocolValue],
        @[label(@"Readiness", @"Readiness label"), _readinessValue],
        @[label(@"Details", @"Validation details label"), _profileDetail],
    ]];
    detailsGrid.rowSpacing = 12.0;
    detailsGrid.columnSpacing = 14.0;
    [detailsGrid columnAtIndex:0].xPlacement = NSGridCellPlacementLeading;
    [detailsGrid columnAtIndex:1].xPlacement = NSGridCellPlacementFill;
    NSStackView* details = [NSStackView stackViewWithViews:@[
        label(@"Profile Details", @"Profile details section"),
        renameRow,
        _enabledCheckbox,
        detailsGrid,
    ]];
    details.orientation = NSUserInterfaceLayoutOrientationVertical;
    details.alignment = NSLayoutAttributeLeading;
    details.spacing = 12.0;
    [renameRow.widthAnchor constraintEqualToAnchor:details.widthAnchor].active = YES;
    [detailsGrid.widthAnchor constraintEqualToAnchor:details.widthAnchor].active = YES;

    NSSplitView* split = [[NSSplitView alloc] initWithFrame:NSZeroRect];
    split.vertical = YES;
    split.dividerStyle = NSSplitViewDividerStyleThin;
    [split addArrangedSubview:profiles];
    [split addArrangedSubview:details];
    [profiles.widthAnchor constraintGreaterThanOrEqualToConstant:275.0].active = YES;
    [details.widthAnchor constraintGreaterThanOrEqualToConstant:330.0].active = YES;

    _commandStatus = label(@"", @"Command status");
    _commandStatus.lineBreakMode = NSLineBreakByTruncatingTail;
    _applyButton = push_button(@"Apply", self, @selector(applyDraft:), @"Apply Profile changes");
    _discardButton = push_button(@"Discard", self, @selector(discardDraft:), @"Discard Profile changes");
    NSStackView* footer = [NSStackView stackViewWithViews:@[
        _commandStatus, _applyButton, _discardButton]];
    footer.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    footer.alignment = NSLayoutAttributeCenterY;
    footer.spacing = 8.0;
    [_commandStatus setContentHuggingPriority:NSLayoutPriorityDefaultLow
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_applyButton setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_discardButton setContentHuggingPriority:NSLayoutPriorityRequired
        forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView* root = [NSStackView stackViewWithViews:@[serviceRow, split, footer]];
    root.translatesAutoresizingMaskIntoConstraints = NO;
    root.orientation = NSUserInterfaceLayoutOrientationVertical;
    root.alignment = NSLayoutAttributeLeading;
    root.spacing = 14.0;
    NSView* content = [[NSView alloc] initWithFrame:NSZeroRect];
    window.contentView = content;
    [content addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16.0],
        [root.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-16.0],
        [root.topAnchor constraintEqualToAnchor:content.topAnchor constant:16.0],
        [root.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-16.0],
        [serviceRow.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [split.widthAnchor constraintEqualToAnchor:root.widthAnchor],
        [footer.widthAnchor constraintEqualToAnchor:root.widthAnchor],
    ]];

    window.defaultButtonCell = _applyButton.cell;
    [self configureKeyLoop];
    return self;
}

- (void)dealloc {
    if (_resourceCounted) {
        --live_controller_count;
    }
}

- (void)configureKeyLoop {
    _newProfileField.nextKeyView = _addButton;
    _addButton.nextKeyView = _removeButton;
    _removeButton.nextKeyView = _profileTable;
    _profileTable.nextKeyView = _renameField;
    _renameField.nextKeyView = _renameButton;
    _renameButton.nextKeyView = _enabledCheckbox;
    _enabledCheckbox.nextKeyView = _applyButton;
    _applyButton.nextKeyView = _discardButton;
    _discardButton.nextKeyView = _newProfileField;
}

- (void)invalidateOwner {
    _owner = nullptr;
    self.window.delegate = nil;
    _profileTable.dataSource = nil;
    _profileTable.delegate = nil;
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
    if (!state->selected_profile_id || *state->selected_profile_id != profile.id) {
        _owner->submit({ccs::MainWindowCommand::SelectProfile, profile.id});
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    if (_owner != nullptr) {
        _owner->request_window_close();
    }
    return NO;
}

- (void)render {
    if (_owner == nullptr) {
        return;
    }
    const auto state = _owner->state();
    if (state == nullptr) {
        return;
    }
    _updating = YES;
    _serviceStatus.stringValue = [NSString stringWithFormat:@"Service: %s",
        ccs::application_state_name(state->application.state)];
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
    if (state->selected_profile_id) {
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
        _renameField.stringValue = @"";
        _enabledCheckbox.state = NSControlStateValueOff;
        _protocolValue.stringValue = @"No Profile selected";
        _readinessValue.stringValue = @"";
        _profileDetail.stringValue = @"";
    } else {
        if (utf8_string(_renameField.stringValue) != profile->id) {
            _renameField.stringValue = ns_string(profile->id);
        }
        _enabledCheckbox.state = profile->enabled
            ? NSControlStateValueOn
            : NSControlStateValueOff;
        _protocolValue.stringValue = profile->protocol
            ? ns_string(*profile->protocol)
            : @"Not configured";
        _readinessValue.stringValue = readiness_text(profile->readiness);
        _profileDetail.stringValue = ns_string(profile->status_detail);
    }

    if (state->command_pending) {
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

    const bool pending = state->command_pending;
    const auto service_actions = ccs::service_actions_for(state->application.state);
    _startButton.enabled = !pending && service_actions.can_start;
    _stopButton.enabled = !pending && service_actions.can_stop;
    _reloadButton.enabled = !pending && service_actions.can_reload;
    _lightweightCheckbox.enabled = !pending;
    _profileTable.enabled = !pending;
    _newProfileField.enabled = !pending;
    _addButton.enabled = !pending;
    const bool has_profile = profile != nullptr;
    _removeButton.enabled = !pending && has_profile;
    _renameField.enabled = !pending && has_profile;
    _renameButton.enabled = !pending && has_profile;
    _enabledCheckbox.enabled = !pending && has_profile;
    _applyButton.enabled = !pending && state->draft.dirty();
    _discardButton.enabled = !pending && state->draft.dirty();
    _updating = NO;
}

- (void)setLocalStatus:(NSString*)message error:(BOOL)error {
    _commandStatus.stringValue = message == nil ? @"" : message;
    _commandStatus.textColor = error ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
}

- (BOOL)validateLayout:(std::string&)error {
    error.clear();
    [self.window.contentView layoutSubtreeIfNeeded];
    if (self.window.contentView.hasAmbiguousLayout) {
        error = "main window content has an ambiguous Auto Layout result";
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
    const NSArray<NSView*>* required_controls = @[
        _serviceStatus, _listenerStatus, _startButton, _stopButton, _reloadButton,
        _lightweightCheckbox, _profileTable, _newProfileField, _addButton, _removeButton,
        _renameField, _renameButton, _enabledCheckbox, _protocolValue, _readinessValue,
        _profileDetail, _commandStatus, _applyButton, _discardButton,
    ];
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
    NSSet<NSView*>* required = [NSSet setWithArray:@[
        _newProfileField, _addButton, _removeButton, _profileTable, _renameField,
        _renameButton, _enabledCheckbox, _applyButton, _discardButton,
    ]];
    NSMutableSet<NSView*>* visited = [NSMutableSet set];
    NSView* current = _newProfileField;
    for (NSInteger step = 0; current != nil && step < 64; ++step) {
        [visited addObject:current];
        current = current.nextKeyView;
        if (current == _newProfileField) {
            break;
        }
    }
    if (current != _newProfileField || ![required isSubsetOfSet:visited]) {
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
            _owner->submit({ccs::MainWindowCommand::RemoveProfile, *state->selected_profile_id});
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
    _owner->submit({
        ccs::MainWindowCommand::RenameProfile,
        *state->selected_profile_id,
        replacement,
    });
}

- (void)toggleProfileEnabled:(id)sender {
    (void)sender;
    if (_owner == nullptr || _updating) return;
    const auto state = _owner->state();
    if (state && state->selected_profile_id) {
        _owner->submit({
            ccs::MainWindowCommand::SetProfileEnabled,
            *state->selected_profile_id,
            {},
            _enabledCheckbox.state == NSControlStateValueOn,
        });
    }
}

- (void)applyDraft:(id)sender {
    (void)sender;
    if (_owner != nullptr) _owner->submit({ccs::MainWindowCommand::ApplyDraft});
}

- (void)discardDraft:(id)sender {
    (void)sender;
    if (_owner != nullptr) _owner->submit({ccs::MainWindowCommand::DiscardDraft});
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
    if (controller_ == nil) {
        controller_ = [[CCSMainWindowController alloc] initWithOwner:this];
        if (controller_ == nil || controller_.window == nil) {
            controller_ = nil;
            error = "failed to create the AppKit main window";
            return false;
        }
        notify_lifecycle("created");
    }
    [controller_ render];
    [controller_ showWindow:nil];
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
    if (!state_ || !state_->selected_profile_id) {
        return nullptr;
    }
    return find_profile_list_item(*state_, *state_->selected_profile_id);
}

void MacMainWindow::Impl::submit(MainWindowCommandRequest request) {
    if (controller_ != nil) {
        [controller_ setLocalStatus:@"" error:NO];
    }
    if (!view_model_.submit(std::move(request)) && controller_ != nil) {
        [controller_ setLocalStatus:@"Another command is already running." error:YES];
    }
}

void MacMainWindow::Impl::request_window_close(
    std::optional<UnsavedChangesDecision> decision) {
    state_ = view_model_.snapshot();
    if (!state_) {
        perform_close(CloseTarget::Destroy);
        return;
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
