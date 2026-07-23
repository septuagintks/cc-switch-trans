#include "controllers/command_dispatcher.hpp"
#include "diagnostics/frame_monitor.hpp"
#include "features/migration/migration_controller.hpp"
#include "features/profiles/profiles_controller.hpp"
#include "features/rules/rules_controller.hpp"
#include "features/settings/settings_controller.hpp"
#include "interaction/animation_policy.hpp"
#include "ipc/bootstrap_reader.hpp"
#include "ipc/gui_ipc_client.hpp"
#include "lifecycle/gui_window_controller.hpp"
#include "prototype/profile_list_model.hpp"
#include "prototype/prototype_controller.hpp"
#include "state/gui_state_store.hpp"

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
#include <QTimer>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <string>

namespace {

bool hasArgument(const QCoreApplication& application, const QString& argument) {
    return application.arguments().contains(argument);
}

void writeInheritedHandle(DWORD handle_id, const QByteArray& bytes) {
    const HANDLE handle = GetStdHandle(handle_id);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    (void)WriteFile(
        handle,
        bytes.constData(),
        static_cast<DWORD>(bytes.size()),
        &written,
        nullptr);
}

void writeError(const QString& error) {
    writeInheritedHandle(STD_ERROR_HANDLE, error.toUtf8() + '\n');
}

void installWarningSink(QQmlApplicationEngine& engine) {
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::warnings,
        [](const QList<QQmlError>& warnings) {
            for (const auto& warning : warnings) writeError(warning.toString());
        });
}

void writeProbeResult(const QJsonObject& result) {
    QByteArray output = "CCS_TRANS_GUI_PROBE ";
    output += QJsonDocument(result).toJson(QJsonDocument::Compact);
    output += '\n';
    writeInheritedHandle(STD_OUTPUT_HANDLE, output);
}

int runPrototypeProbe(
    QGuiApplication& application,
    QElapsedTimer& startup_timer,
    bool self_test,
    bool lifecycle_probe) {
    ccs_trans::gui::AnimationPolicy motion_policy;
    ccs_trans::gui::ProfileListModel profile_model;
    profile_model.populate(128, 64);
    ccs_trans::gui::PrototypeController controller(&profile_model);
    controller.setSelectedKey(profile_model.stableKeyAt(0));

    QQmlApplicationEngine engine;
    installWarningSink(engine);
    engine.rootContext()->setContextProperty(
        QStringLiteral("motionPolicy"), &motion_policy);
    engine.rootContext()->setContextProperty(
        QStringLiteral("profileModel"), &profile_model);
    engine.rootContext()->setContextProperty(
        QStringLiteral("prototypeController"), &controller);
    engine.loadFromModule(
        QStringLiteral("CcsTrans.Gui"), QStringLiteral("PrototypeProbe"));
    if (engine.rootObjects().isEmpty()) return EXIT_FAILURE;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (window == nullptr) return EXIT_FAILURE;

    ccs_trans::gui::FrameMonitor frame_monitor;
    frame_monitor.attach(window);
    if (lifecycle_probe) {
        QTimer::singleShot(100, &application, [&] {
            application.exit(EXIT_SUCCESS);
        });
    }
    if (self_test) {
        const qint64 startup_milliseconds = startup_timer.elapsed();
        QTimer::singleShot(150, &application, [&] {
            controller.startStress(4096);
        });
        QObject::connect(
            &controller,
            &ccs_trans::gui::PrototypeController::stressFinished,
            &application,
            [&](qint64 mutation_milliseconds) {
                const bool model_valid = profile_model.count() == 128
                    && profile_model.totalRuleCount() == 8192
                    && profile_model.hasUniqueStableKeys();
                const bool selection_stable = profile_model.containsStableKey(
                    controller.selectedKey());
                QTimer::singleShot(
                    motion_policy.movementDuration() + 150,
                    &application,
                    [&, model_valid, selection_stable, mutation_milliseconds,
                     startup_milliseconds] {
                        const auto idle_start = frame_monitor.frameCount();
                        QTimer::singleShot(1000, &application, [&, model_valid,
                            selection_stable, mutation_milliseconds,
                            startup_milliseconds, idle_start] {
                            const auto idle_frames =
                                frame_monitor.frameCount() - idle_start;
                            const bool idle_stable = idle_frames <= 2;
                            writeProbeResult({
                                {QStringLiteral("version"),
                                 QStringLiteral(CCS_TRANS_GUI_VERSION)},
                                {QStringLiteral("source_commit"),
                                 QStringLiteral(CCS_TRANS_GUI_SOURCE_COMMIT)},
                                {QStringLiteral("graphics_api"),
                                 frame_monitor.graphicsApiName()},
                                {QStringLiteral("startup_ms"), startup_milliseconds},
                                {QStringLiteral("mutation_ms"), mutation_milliseconds},
                                {QStringLiteral("profiles"), profile_model.count()},
                                {QStringLiteral("rules"), profile_model.totalRuleCount()},
                                {QStringLiteral("mutations"),
                                 controller.completedMutations()},
                                {QStringLiteral("selection_stable"), selection_stable},
                                {QStringLiteral("model_valid"), model_valid},
                                {QStringLiteral("idle_frames_1s"),
                                 static_cast<qint64>(idle_frames)},
                                {QStringLiteral("idle_stable"), idle_stable},
                            });
                            application.exit(model_valid && selection_stable
                                    && idle_stable
                                ? EXIT_SUCCESS : EXIT_FAILURE);
                        });
                    });
            });
        QTimer::singleShot(10000, &application, [&] {
            application.exit(EXIT_FAILURE);
        });
    }
    return application.exec();
}

int runProduction(
    QGuiApplication& application,
    ccs::gui_ipc::LaunchBootstrap bootstrap,
    bool qml_probe = false) {
    ccs_trans::gui::AnimationPolicy motion_policy;
    ccs_trans::gui::GuiIpcClient client(std::move(bootstrap));
    ccs_trans::gui::GuiStateStore state(client);
    ccs_trans::gui::CommandDispatcher commands(client);
    ccs_trans::gui::ProfilesController profiles(state, commands);
    ccs_trans::gui::RulesController rules(state, commands);
    ccs_trans::gui::SettingsController settings(state, commands);
    ccs_trans::gui::MigrationController migration(state, commands);
    ccs_trans::gui::GuiWindowController window_controller(
        application, client, state, commands);

    QQmlApplicationEngine engine;
    installWarningSink(engine);
    auto* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("motionPolicy"), &motion_policy);
    context->setContextProperty(QStringLiteral("ipcClient"), &client);
    context->setContextProperty(QStringLiteral("guiState"), &state);
    context->setContextProperty(QStringLiteral("commandDispatcher"), &commands);
    context->setContextProperty(QStringLiteral("profilesController"), &profiles);
    context->setContextProperty(QStringLiteral("rulesController"), &rules);
    context->setContextProperty(QStringLiteral("settingsController"), &settings);
    context->setContextProperty(QStringLiteral("migrationController"), &migration);
    context->setContextProperty(QStringLiteral("windowController"), &window_controller);
    engine.loadFromModule(QStringLiteral("CcsTrans.Gui"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return EXIT_FAILURE;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (window == nullptr) return EXIT_FAILURE;

    window_controller.attachWindow(window);
    ccs_trans::gui::FrameMonitor frame_monitor;
    frame_monitor.attach(window);
    if (qml_probe) {
        QTimer::singleShot(100, &application, [&application] {
            application.exit(EXIT_SUCCESS);
        });
    } else {
        client.start();
    }
    return application.exec();
}

} // namespace

int main(int argc, char* argv[]) {
    QElapsedTimer startup_timer;
    startup_timer.start();
    QGuiApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("ccs-trans"));
    application.setApplicationVersion(QStringLiteral(CCS_TRANS_GUI_VERSION));
    application.setOrganizationName(QStringLiteral("ccs-trans"));
    application.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    if (hasArgument(application, QStringLiteral("--software-renderer"))) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }
    const bool self_test = hasArgument(application, QStringLiteral("--self-test"));
    const bool lifecycle_probe =
        hasArgument(application, QStringLiteral("--lifecycle-probe"));
    if (self_test || lifecycle_probe) {
        return runPrototypeProbe(
            application, startup_timer, self_test, lifecycle_probe);
    }
    if (hasArgument(application, QStringLiteral("--production-qml-probe"))) {
        return runProduction(
            application,
            {
                "probe-pipe",
                CCS_TRANS_GUI_VERSION,
                CCS_TRANS_GUI_SOURCE_COMMIT,
                "probe-instance",
                "probe-token",
                "probe-session",
            },
            true);
    }

    std::uintptr_t inherited_handle = 0;
    QString argument_error;
    if (!ccs_trans::gui::bootstrap_handle_from_arguments(
            application.arguments(), inherited_handle, argument_error)) {
        writeError(argument_error);
        return EXIT_FAILURE;
    }
    ccs::gui_ipc::LaunchBootstrap bootstrap;
    std::string bootstrap_error;
    if (!ccs_trans::gui::read_launch_bootstrap(
            inherited_handle, bootstrap, bootstrap_error)) {
        writeError(QString::fromUtf8(bootstrap_error));
        return EXIT_FAILURE;
    }
    if (bootstrap.version != CCS_TRANS_GUI_VERSION
        || bootstrap.source_commit != CCS_TRANS_GUI_SOURCE_COMMIT) {
        writeError(QStringLiteral("GUI bootstrap version or source commit mismatch"));
        return EXIT_FAILURE;
    }
    return runProduction(application, std::move(bootstrap));
}
