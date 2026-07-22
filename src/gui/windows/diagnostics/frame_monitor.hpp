#pragma once

#include <QObject>
#include <QString>

#include <cstdint>

class QQuickWindow;

namespace ccs_trans::gui {

class FrameMonitor final : public QObject {
    Q_OBJECT

public:
    explicit FrameMonitor(QObject* parent = nullptr);

    void attach(QQuickWindow* window);
    [[nodiscard]] std::uint64_t frameCount() const noexcept;
    [[nodiscard]] QString graphicsApiName() const;

private:
    QQuickWindow* window_{nullptr};
    std::uint64_t frameCount_{0};
};

}  // namespace ccs_trans::gui
