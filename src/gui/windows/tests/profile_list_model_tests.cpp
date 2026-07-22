#include "interaction/animation_policy.hpp"
#include "prototype/profile_list_model.hpp"
#include "prototype/prototype_controller.hpp"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

namespace ccs_trans::gui {

class ProfileListModelTests final : public QObject {
    Q_OBJECT

private slots:
    void initializesDesktopStressFixture();
    void incrementalMutationsPreserveStableSelection();
    void asynchronousStressYieldsAndCompletes();
    void reduceMotionKeepsDeterministicEndState();
};

void ProfileListModelTests::initializesDesktopStressFixture() {
    ProfileListModel model;
    QElapsedTimer timer;
    timer.start();
    model.populate(128, 64);

    QCOMPARE(model.count(), 128);
    QCOMPARE(model.totalRuleCount(), 8'192);
    QVERIFY(model.hasUniqueStableKeys());
    QVERIFY2(timer.elapsed() < 250, "128/8192 model initialization exceeded 250 ms");
}

void ProfileListModelTests::incrementalMutationsPreserveStableSelection() {
    ProfileListModel model;
    model.populate(128, 64);
    const QString selectedKey = model.stableKeyAt(31);
    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);

    for (std::uint64_t mutation = 0; mutation < 16'384; ++mutation) {
        model.applyMutation(mutation, selectedKey);
    }

    QCOMPARE(resetSpy.count(), 0);
    QCOMPARE(model.count(), 128);
    QCOMPARE(model.totalRuleCount(), 8'192);
    QVERIFY(model.containsStableKey(selectedKey));
    QVERIFY(model.hasUniqueStableKeys());
}

void ProfileListModelTests::asynchronousStressYieldsAndCompletes() {
    ProfileListModel model;
    model.populate(128, 64);
    PrototypeController controller(&model);
    controller.setSelectedKey(model.stableKeyAt(7));
    const QString selectedKey = controller.selectedKey();
    QSignalSpy finishedSpy(&controller, &PrototypeController::stressFinished);
    QSignalSpy progressSpy(&controller, &PrototypeController::completedMutationsChanged);

    controller.startStress(4'096);
    QVERIFY(controller.stressRunning());
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5'000);

    QVERIFY(!controller.stressRunning());
    QCOMPARE(controller.completedMutations(), 4'096);
    QVERIFY2(progressSpy.count() > 2, "model synchronization did not yield between batches");
    QCOMPARE(controller.selectedKey(), selectedKey);
    QVERIFY(model.containsStableKey(selectedKey));
}

void ProfileListModelTests::reduceMotionKeepsDeterministicEndState() {
    AnimationPolicy policy;
    policy.setReduceMotion(true);
    QCOMPARE(policy.shortDuration(), 0);
    QCOMPARE(policy.mediumDuration(), 0);
    QCOMPARE(policy.movementDuration(), 0);

    policy.setReduceMotion(false);
    QVERIFY(policy.shortDuration() > 0);
    QVERIFY(policy.mediumDuration() >= policy.shortDuration());
    QVERIFY(policy.movementDuration() >= policy.mediumDuration());
}

}  // namespace ccs_trans::gui

QTEST_MAIN(ccs_trans::gui::ProfileListModelTests)
#include "profile_list_model_tests.moc"
