#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QRectF>
#include <QSizeF>
#include <QStringList>
#include <atomic>

struct ScanDevice {
    QString name;    // SANE device name, e.g. "canon_dr:libusb:003:006"
    QString key;     // stable across replugging: vendor + model
    QString label;   // shown to the user
};

struct DeviceCaps {
    QString device;
    QStringList sources;   // raw SANE values
    QStringList modes;     // raw SANE values
    QList<int> resolutions;
    QSizeF maxMm;          // largest scan area
    QSizeF flatbedMm;      // glass size if it differs (eSCL); empty = maxMm
};

struct ScanSettings {
    QString device;
    QString source;
    QString mode = QStringLiteral("Color");
    int dpi = 300;
    QSizeF paperMm{210.0, 297.0};
    QRectF areaMm;          // flatbed: scan only this area (empty = paperMm)
    bool preview = false;   // quick low-resolution scan of the whole glass
    bool deskew = true;     // hardware deskew where the scanner has it (canon_dr)
};

// Scanner access, running in its own thread. The GUI only uses this interface;
// the implementation is platform specific:
//   scanworker_sane.cpp  – Linux/BSD/macOS via SANE
//   scanworker_win.cpp   – Windows (TWAIN/WIA), to be written
// Network scanners (eSCL/AirScan) are handled by esclclient.cpp on all platforms.
//
// Source and mode strings use the SANE vocabulary ("ADF Duplex", "ADF Front",
// "Flatbed", "Color", "Gray", "Lineart", …); other backends map onto it so the
// GUI's labels and isFeeder() work unchanged.
class ScanWorker : public QObject
{
    Q_OBJECT
public:
    explicit ScanWorker(QObject *parent = nullptr) : QObject(parent) {}

    void requestCancel() { m_cancel = true; }

    static bool isFeeder(const QString &source);

public slots:
    void listDevices();
    void queryCaps(const QString &device);
    void scan(const ScanSettings &settings);

signals:
    void status(const QString &text);
    void devicesFound(const QList<ScanDevice> &devices);
    void capsReady(const DeviceCaps &caps, const QString &error);
    void pageScanned(const QImage &image);
    // live view while a page is being read: the part received so far
    // (scaled down) and the expected full height in the same scale
    void pageProgress(const QImage &partial, int expectedHeight);
    void previewReady(const QImage &image);
    // something the user has to do (e.g. a missing firmware file): shown in
    // an info window rather than in the status line
    void notice(const QString &title, const QString &text);
    void finished(int pages, const QString &error);

private:
    std::atomic_bool m_cancel{false};
    DeviceCaps m_lastCaps;   // needed by the eSCL client when scanning
};

Q_DECLARE_METATYPE(ScanDevice)
Q_DECLARE_METATYPE(DeviceCaps)
Q_DECLARE_METATYPE(ScanSettings)
