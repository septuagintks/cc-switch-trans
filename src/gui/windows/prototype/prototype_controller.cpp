#include "prototype/prototype_controller.hpp"

#include "prototype/profile_list_model.hpp"

#include <QDateTime>
#include <QElapsedTimer>

#include <algorithm>

namespace ccs_trans::gui {

PrototypeController::PrototypeController(ProfileListModel* model, QObject* parent)
    : QObject(parent), model_(model) {
    stressTimer_.setInterval(0);
    stressTimer_.setTimerType(Qt::PreciseTimer);
    connect(&stressTimer_, &QTimer::timeout, this, &PrototypeController::runStressBatch);
    connect(model_, &QAbstractItemModel::rowsInserted,
            this, &PrototypeController::refreshSelectionIndex);
    connect(model_, &QAbstractItemModel::rowsRemoved,
            this, &PrototypeController::refreshSelectionIndex);
    connect(model_, &QAbstractItemModel::rowsMoved,
            this, &PrototypeController::refreshSelectionIndex);
    connect(model_, &QAbstractItemModel::modelReset,
            this, &PrototypeController::refreshSelectionIndex);
}

QString PrototypeController::selectedKey() const {
    return selectedKey_;
}

int PrototypeController::selectedIndex() const {
    return model_->indexOfStableKey(selectedKey_);
}

bool PrototypeController::stressRunning() const noexcept {
    return stressTimer_.isActive();
}

int PrototypeController::completedMutations() const noexcept {
    return completedMutations_;
}

void PrototypeController::setSelectedKey(const QString& selectedKey) {
    if (selectedKey_ == selectedKey || !model_->containsStableKey(selectedKey)) {
        return;
    }
    selectedKey_ = selectedKey;
    emit selectedKeyChanged();
    emit selectedIndexChanged();
}

void PrototypeController::startStress(const int mutationCount) {
    if (stressRunning() || mutationCount <= 0 || model_->count() == 0) {
        return;
    }
    if (selectedKey_.isEmpty()) {
        setSelectedKey(model_->stableKeyAt(0));
    }
    remainingMutations_ = std::min(mutationCount, 1'000'000);
    completedMutations_ = 0;
    stressStartedAt_ = QDateTime::currentMSecsSinceEpoch();
    emit completedMutationsChanged();
    stressTimer_.start();
    emit stressRunningChanged();
}

void PrototypeController::runStressBatch() {
    constexpr int maximumBatchSize = 32;
    constexpr qint64 batchBudgetNanoseconds = 2'000'000;
    QElapsedTimer batchTimer;
    batchTimer.start();

    int completedInBatch = 0;
    const int availableInBatch = std::min(remainingMutations_, maximumBatchSize);
    while (completedInBatch < availableInBatch &&
           (completedInBatch == 0 || batchTimer.nsecsElapsed() < batchBudgetNanoseconds)) {
        model_->applyMutation(mutationSequence_++, selectedKey_);
        ++completedInBatch;
    }
    remainingMutations_ -= completedInBatch;
    completedMutations_ += completedInBatch;
    emit completedMutationsChanged();

    if (remainingMutations_ > 0) {
        return;
    }
    stressTimer_.stop();
    emit stressRunningChanged();
    emit stressFinished(QDateTime::currentMSecsSinceEpoch() - stressStartedAt_);
}

void PrototypeController::refreshSelectionIndex() {
    emit selectedIndexChanged();
}

}  // namespace ccs_trans::gui
