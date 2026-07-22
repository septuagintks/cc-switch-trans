#pragma once

#include <QObject>

namespace ccs_trans::gui {

class AnimationPolicy final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool reduceMotion READ reduceMotion WRITE setReduceMotion NOTIFY reduceMotionChanged)
    Q_PROPERTY(bool highContrast READ highContrast CONSTANT)
    Q_PROPERTY(int shortDuration READ shortDuration NOTIFY reduceMotionChanged)
    Q_PROPERTY(int mediumDuration READ mediumDuration NOTIFY reduceMotionChanged)
    Q_PROPERTY(int movementDuration READ movementDuration NOTIFY reduceMotionChanged)

public:
    explicit AnimationPolicy(QObject* parent = nullptr);

    [[nodiscard]] bool reduceMotion() const noexcept;
    [[nodiscard]] bool highContrast() const noexcept;
    [[nodiscard]] int shortDuration() const noexcept;
    [[nodiscard]] int mediumDuration() const noexcept;
    [[nodiscard]] int movementDuration() const noexcept;

public slots:
    void setReduceMotion(bool reduceMotion);

signals:
    void reduceMotionChanged();

private:
    bool reduceMotion_{false};
    bool highContrast_{false};
};

}  // namespace ccs_trans::gui
