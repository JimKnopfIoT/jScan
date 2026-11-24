#include "scancontroller.h"

#include <sailfishapp.h>

#include <QGuiApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>
#include <QScopedPointer>

int main(int argc, char *argv[])
{
    // the statically linked SANE (canon_dr) reads its device list from here
    qputenv("SANE_CONFIG_DIR", QByteArrayLiteral("/usr/share/jscan/sane"));
    // bundled Tesseract (OCR) and its language data
    qputenv("TESSDATA_PREFIX", QByteArrayLiteral("/usr/share/jscan/tessdata"));
    qputenv("PATH", QByteArrayLiteral("/usr/share/jscan/bin:") + qgetenv("PATH"));

    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    app->setApplicationName(QStringLiteral("jscan"));
    app->setOrganizationName(QStringLiteral("jscan"));

    qRegisterMetaType<ScanSettings>();
    qRegisterMetaType<ScanDevice>();
    qRegisterMetaType<QList<ScanDevice>>();
    qRegisterMetaType<DeviceCaps>();

    ScanController controller;

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    view->engine()->addImageProvider(QStringLiteral("jscan"), new PageImageProvider(&controller));
    view->rootContext()->setContextProperty(QStringLiteral("scanner"), &controller);
    view->rootContext()->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(APP_VERSION));
    view->setSource(SailfishApp::pathTo(QStringLiteral("qml/jscan.qml")));
    view->showFullScreen();   // show() gives a white first start under Lipstick
    return app->exec();
}
