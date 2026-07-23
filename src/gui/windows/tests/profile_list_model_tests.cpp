#include "interaction/animation_policy.hpp"
#include "models/editable_field_model.hpp"
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
    void editableFieldPreservesInvalidTextAcrossSnapshots();
    void editableFieldAppliesIncrementalDescriptorChanges();
    void editableFieldRebasesEditsMadeAfterSubmit();
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

void ProfileListModelTests::editableFieldPreservesInvalidTextAcrossSnapshots() {
    EditableFieldModel model;
    ccs::gui_ipc::FieldState port;
    port.key = "listener.port";
    port.scope = "application";
    port.input_kind = "unsigned_integer";
    port.required = true;
    port.minimum = 1;
    port.maximum = 65'535;
    port.value = std::uint64_t{15'723};
    model.apply({port});

    QVERIFY(!model.setValue(QStringLiteral("listener.port"), QStringLiteral("broken")));
    QCOMPARE(model.data(model.index(0), EditableFieldModel::DraftTextRole).toString(),
        QStringLiteral("broken"));
    QVERIFY(model.dirty());
    QVERIFY(!model.valid());
    QVERIFY(model.edits().empty());

    port.value = std::uint64_t{16'000};
    model.apply({port});
    QCOMPARE(model.data(model.index(0), EditableFieldModel::DraftTextRole).toString(),
        QStringLiteral("broken"));
    QVERIFY(model.dirty());

    model.discardLocal();
    QCOMPARE(model.data(model.index(0), EditableFieldModel::DraftTextRole).toString(),
        QStringLiteral("16000"));
    QVERIFY(!model.dirty());
    QVERIFY(model.valid());
}

void ProfileListModelTests::editableFieldAppliesIncrementalDescriptorChanges() {
    EditableFieldModel model;
    ccs::gui_ipc::FieldState first;
    first.key = "id";
    first.scope = "profile";
    first.input_kind = "text";
    first.value = std::string{"alpha"};
    ccs::gui_ipc::FieldState second = first;
    second.key = "local.request-path";
    second.value = std::string{"/alpha/responses"};
    model.apply({first, second});
    QVERIFY(model.setValue(QStringLiteral("id"), QStringLiteral("local-alpha")));

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    QSignalSpy moveSpy(&model, &QAbstractItemModel::rowsMoved);
    QSignalSpy insertSpy(&model, &QAbstractItemModel::rowsInserted);
    first.value = std::string{"server-alpha"};
    ccs::gui_ipc::FieldState third = second;
    third.key = "upstream.base-url";
    third.input_kind = "url";
    third.value = std::string{"https://example.test"};
    model.apply({second, first, third});

    QCOMPARE(resetSpy.count(), 0);
    QVERIFY(moveSpy.count() >= 1);
    QCOMPARE(insertSpy.count(), 1);
    QCOMPARE(model.count(), 3);
    QCOMPARE(model.textValue(QStringLiteral("id")), QStringLiteral("local-alpha"));
    QVERIFY(model.dirty());
    QCOMPARE(model.edits().size(), std::size_t{1});
}

void ProfileListModelTests::editableFieldRebasesEditsMadeAfterSubmit() {
    EditableFieldModel model;
    ccs::gui_ipc::FieldState field;
    field.key = "id";
    field.scope = "profile";
    field.input_kind = "text";
    field.value = std::string{"alpha"};
    model.apply({field});

    QVERIFY(model.setValue(QStringLiteral("id"), QStringLiteral("beta")));
    const auto submitted_revision = model.localRevision();

    // Returning to the old server value is still a post-submit edit. Once the
    // server confirms beta, the local alpha must survive and become dirty.
    QVERIFY(model.setValue(QStringLiteral("id"), QStringLiteral("alpha")));
    QVERIFY(!model.dirty());
    field.value = std::string{"beta"};
    model.apply({field}, false, submitted_revision);

    QCOMPARE(model.textValue(QStringLiteral("id")), QStringLiteral("alpha"));
    QVERIFY(model.dirty());
    QCOMPARE(model.edits().size(), std::size_t{1});
    QCOMPARE(std::get<std::string>(*model.edits().front().value),
        std::string("alpha"));
}

}  // namespace ccs_trans::gui

QTEST_MAIN(ccs_trans::gui::ProfileListModelTests)
#include "profile_list_model_tests.moc"
