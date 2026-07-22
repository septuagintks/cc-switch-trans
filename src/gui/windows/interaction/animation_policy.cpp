#include "interaction/animation_policy.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ccs_trans::gui {

namespace {

bool systemAnimationsEnabled() {
    BOOL enabled = TRUE;
    return SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0) != FALSE &&
           enabled != FALSE;
}

bool systemHighContrastEnabled() {
    HIGHCONTRASTW value{};
    value.cbSize = sizeof(value);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0) != FALSE &&
           (value.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

}  // namespace

AnimationPolicy::AnimationPolicy(QObject* parent)
    : QObject(parent),
      reduceMotion_(!systemAnimationsEnabled()),
      highContrast_(systemHighContrastEnabled()) {}

bool AnimationPolicy::reduceMotion() const noexcept {
    return reduceMotion_;
}

bool AnimationPolicy::highContrast() const noexcept {
    return highContrast_;
}

int AnimationPolicy::shortDuration() const noexcept {
    return reduceMotion_ ? 0 : 110;
}

int AnimationPolicy::mediumDuration() const noexcept {
    return reduceMotion_ ? 0 : 170;
}

int AnimationPolicy::movementDuration() const noexcept {
    return reduceMotion_ ? 0 : 220;
}

void AnimationPolicy::setReduceMotion(const bool reduceMotion) {
    if (reduceMotion_ == reduceMotion) {
        return;
    }
    reduceMotion_ = reduceMotion;
    emit reduceMotionChanged();
}

}  // namespace ccs_trans::gui
