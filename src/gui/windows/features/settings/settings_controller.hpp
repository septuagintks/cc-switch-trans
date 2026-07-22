#pragma once

#include <QObject>

namespace ccs_trans::gui {

class CommandDispatcher;
class GuiStateStore;

class SettingsController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool lightweightMode READ lightweightMode NOTIFY lightweightModeChanged)

public:
    SettingsController(
        GuiStateStore& state,
        CommandDispatcher& commands,
        QObject* parent = nullptr);

    [[nodiscard]] bool lightweightMode() const noexcept;
    Q_INVOKABLE void setLightweightMode(bool enabled);

signals:
    void lightweightModeChanged();

private:
    GuiStateStore& state_;
    CommandDispatcher& commands_;
};

} // namespace ccs_trans::gui
