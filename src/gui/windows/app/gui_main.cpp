#include "diagnostics/frame_monitor.hpp"
#include "interaction/animation_policy.hpp"
#include "prototype/profile_list_model.hpp"
#include "prototype/prototype_controller.hpp"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QTimer>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>

namespace {

bool hasArgument(const QCoreApplication& application, const QString& argument) {
    return application.arguments().contains(argument);
}

void writeInheritedHandle(const DWORD handleId, const QByteArray& bytes) {
    const HANDLE handle = GetStdHandle(handleId);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    static_cast<void>(WriteFile(handle, bytes.constData(),
                                static_cast<DWORD>(bytes.size()), &written, nullptr));
}

void writeProbeResult(const QJsonObject& result) {
    QByteArray output = "CCS_TRANS_GUI_PROBE ";
    output += QJsonDocument(result).toJson(QJsonDocument::Compact);
    output += '\n';
    writeInheritedHandle(STD_OUTPUT_HANDLE, output);
}

}  // namespace

int main(int argc, char* argv[]) {
    QElapsedTimer startupTimer;
    startupTimer.start();

    QGuiApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("ccs-trans"));
    application.setApplicationVersion(QStringLiteral(CCS_TRANS_GUI_VERSION));
    application.setOrganizationName(QStringLiteral("ccs-trans"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    const bool selfTest = hasArgument(application, QStringLiteral("--self-test"));
    const bool lifecycleProbe =
        hasArgument(application, QStringLiteral("--lifecycle-probe"));
    if (hasArgument(application, QStringLiteral("--software-renderer"))) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    ccs_trans::gui::AnimationPolicy motionPolicy;
    ccs_trans::gui::ProfileListModel profileModel;
    profileModel.populate(128, 64);
    ccs_trans::gui::PrototypeController controller(&profileModel);
    controller.setSelectedKey(profileModel.stableKeyAt(0));

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     [](const QList<QQmlError>& warnings) {
                         for (const auto& warning : warnings) {
                             writeInheritedHandle(STD_ERROR_HANDLE,
                                                  warning.toString().toUtf8() + '\n');
                         }
                     });
    engine.rootContext()->setContextProperty(QStringLiteral("motionPolicy"), &motionPolicy);
    engine.rootContext()->setContextProperty(QStringLiteral("profileModel"), &profileModel);
    engine.rootContext()->setContextProperty(QStringLiteral("prototypeController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("prototypeSelfTest"), selfTest);
    engine.loadFromModule(QStringLiteral("CcsTrans.Gui"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (window == nullptr) {
        return EXIT_FAILURE;
    }
    ccs_trans::gui::FrameMonitor frameMonitor;
    frameMonitor.attach(window);

    if (lifecycleProbe) {
        QTimer::singleShot(100, &application, [&] { application.exit(EXIT_SUCCESS); });
    }

    if (selfTest) {
        const qint64 startupMilliseconds = startupTimer.elapsed();
        QTimer::singleShot(150, &application, [&] { controller.startStress(4'096); });
        QObject::connect(
            &controller,
            &ccs_trans::gui::PrototypeController::stressFinished,
            &application,
            [&](const qint64 mutationMilliseconds) {
                const bool modelValid = profileModel.count() == 128 &&
                                        profileModel.totalRuleCount() == 8'192 &&
                                        profileModel.hasUniqueStableKeys();
                const QString selectedKey = controller.selectedKey();
                const bool selectionStable = profileModel.containsStableKey(selectedKey);

                QTimer::singleShot(motionPolicy.movementDuration() + 150, &application, [&, modelValid,
                                      selectionStable, mutationMilliseconds,
                                      startupMilliseconds] {
                    const std::uint64_t idleStartFrames = frameMonitor.frameCount();
                    QTimer::singleShot(1'000, &application, [&, modelValid, selectionStable,
                                          mutationMilliseconds, startupMilliseconds,
                                          idleStartFrames] {
                        const auto idleFrames = frameMonitor.frameCount() - idleStartFrames;
                        const bool idleStable = idleFrames <= 2;
                        writeProbeResult({
                            {QStringLiteral("version"),
                             QStringLiteral(CCS_TRANS_GUI_VERSION)},
                            {QStringLiteral("source_commit"),
                             QStringLiteral(CCS_TRANS_GUI_SOURCE_COMMIT)},
                            {QStringLiteral("graphics_api"), frameMonitor.graphicsApiName()},
                            {QStringLiteral("startup_ms"), startupMilliseconds},
                            {QStringLiteral("mutation_ms"), mutationMilliseconds},
                            {QStringLiteral("profiles"), profileModel.count()},
                            {QStringLiteral("rules"), profileModel.totalRuleCount()},
                            {QStringLiteral("mutations"), controller.completedMutations()},
                            {QStringLiteral("selection_stable"), selectionStable},
                            {QStringLiteral("model_valid"), modelValid},
                            {QStringLiteral("idle_frames_1s"), static_cast<qint64>(idleFrames)},
                            {QStringLiteral("idle_stable"), idleStable},
                            {QStringLiteral("reduce_motion"), motionPolicy.reduceMotion()},
                            {QStringLiteral("high_contrast"), motionPolicy.highContrast()},
                        });
                        application.exit(modelValid && selectionStable && idleStable
                                             ? EXIT_SUCCESS
                                             : EXIT_FAILURE);
                    });
                });
            });
        QTimer::singleShot(10'000, &application, [&] { application.exit(EXIT_FAILURE); });
    }

    return application.exec();
}
