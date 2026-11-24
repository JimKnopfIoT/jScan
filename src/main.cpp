#include "mainwindow.h"
#include "theme.h"

#include <QApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("jscan"));
    QApplication::setOrganizationName(QStringLiteral("jscan"));
    QApplication::setApplicationDisplayName(QStringLiteral("jScan"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setDesktopFileName(QStringLiteral("jscan"));
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("scanner")));

    // Qt's own texts (Yes/No, file dialog …) in the system language. Looked up
    // in Qt's translation directory and, for a deployed Windows build, next to
    // the executable.
    auto *qtTr = new QTranslator(&app);
    if (qtTr->load(QLocale::system(), QStringLiteral("qtbase"), QStringLiteral("_"),
                   QLibraryInfo::path(QLibraryInfo::TranslationsPath))
        || qtTr->load(QLocale::system(), QStringLiteral("qtbase"), QStringLiteral("_"),
                      QCoreApplication::applicationDirPath() + QStringLiteral("/translations")))
        QApplication::installTranslator(qtTr);

    // jScan's own texts: English source, German from :/i18n/jscan-de.qm
    auto *appTr = new QTranslator(&app);
    if (appTr->load(QLocale::system(), QStringLiteral("jscan"), QStringLiteral("-"),
                    QStringLiteral(":/i18n")))
        QApplication::installTranslator(appTr);

    Theme::apply(app);

    // one-time takeover of the settings from the app's former name
    {
        QSettings now;
        if (now.allKeys().isEmpty()) {
            QSettings old(QStringLiteral("p215scan"), QStringLiteral("p215scan"));
            for (const QString &k : old.allKeys())
                now.setValue(k, old.value(k));
        }
    }

    qRegisterMetaType<ScanSettings>();
    qRegisterMetaType<ScanDevice>();
    qRegisterMetaType<QList<ScanDevice>>();
    qRegisterMetaType<DeviceCaps>();

    MainWindow w;
    w.show();
    return app.exec();
}
