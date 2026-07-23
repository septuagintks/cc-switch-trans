#include <QQmlEngine>
#include <QUrl>
#include <QtQuickTest/quicktest.h>
#include <qqml.h>

class QmlTestSetup final : public QObject {
    Q_OBJECT

public slots:
    void qmlEngineAvailable(QQmlEngine*) {
        qmlRegisterSingletonType(
            QUrl::fromLocalFile(QStringLiteral(CCS_TRANS_GUI_THEME_PATH)),
            "CcsTrans.Gui",
            1,
            0,
            "Theme");
    }
};

QUICK_TEST_MAIN_WITH_SETUP(ccs_trans_gui_qml_tests, QmlTestSetup)

#include "qml_test_main.moc"
