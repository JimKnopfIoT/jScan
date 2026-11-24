#include "esclclient.h"

#include <QCoreApplication>

#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QSslConfiguration>
#include <QThread>
#include <QTimer>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

namespace Escl {

namespace {

struct Reply {
    int code = 0;            // HTTP status, 0 = no answer (network error)
    QByteArray body;
    QUrl location;
    QString error;
};

// One blocking HTTP request with timeout. Self-signed certificates of local
// devices are accepted (there is no way to verify them anyway).
Reply request(QNetworkAccessManager &nam, const QByteArray &verb, const QUrl &url,
              const QByteArray &body = {}, int timeoutMs = 30000)
{
    QNetworkRequest req(url);
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(ssl);
    if (!body.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("text/xml"));

    // QBuffer overload and own timeout timer: also available in Qt 5.6
    auto *buf = new QBuffer;
    buf->setData(body);
    buf->open(QIODevice::ReadOnly);
    QNetworkReply *r = nam.sendCustomRequest(req, verb, buf);
    buf->setParent(r);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, r, &QNetworkReply::abort);
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs);
    loop.exec();
    timer.stop();

    Reply out;
    out.code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    out.body = r->readAll();
    const QByteArray loc = r->rawHeader("Location");
    if (!loc.isEmpty())
        out.location = url.resolved(QUrl(QString::fromUtf8(loc)));
    if (r->error() != QNetworkReply::NoError && out.code == 0)
        out.error = r->errorString();
    r->deleteLater();
    return out;
}

QString modeToEscl(const QString &m)
{
    if (m == QLatin1String("Gray") || m == QLatin1String("Lineart"))
        return QStringLiteral("Grayscale8");   // Lineart: thresholded locally
    return QStringLiteral("RGB24");
}

int toEsclUnits(double mm)   // 1/300 inch
{
    return int(std::lround(mm / 25.4 * 300.0));
}

QImage toLineart(const QImage &img)
{
    const QImage g = img.convertToFormat(QImage::Format_Grayscale8);
    QImage m(g.size(), QImage::Format_Mono);
    m.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});
    m.fill(0);
    for (int y = 0; y < g.height(); ++y) {
        const uchar *s = g.constScanLine(y);
        uchar *d = m.scanLine(y);
        for (int x = 0; x < g.width(); ++x)
            if (s[x] < 160)
                d[x >> 3] |= uchar(0x80 >> (x & 7));
    }
    m.setDotsPerMeterX(img.dotsPerMeterX());
    m.setDotsPerMeterY(img.dotsPerMeterY());
    return m;
}

void sleepMs(int ms, const std::atomic_bool &cancel)
{
    for (int t = 0; t < ms && !cancel; t += 100)
        QThread::msleep(100);
}

} // namespace

bool handles(const QString &deviceName)
{
    return deviceName.startsWith(QLatin1String("escl:http"));
}

QUrl baseUrl(const QString &deviceName)
{
    QUrl u(deviceName.mid(5));   // strip "escl:"
    QString path = u.path();
    if (!path.endsWith(QLatin1Char('/')))
        path += QLatin1Char('/');
    if (!path.contains(QLatin1String("eSCL")))
        path += QLatin1String("eSCL/");
    u.setPath(path);
    return u;
}

QString queryCaps(const QString &deviceName, DeviceCaps *caps)
{
    QNetworkAccessManager nam;
    const QUrl base = baseUrl(deviceName);

    Reply r;
    for (int attempt = 0; attempt < 4; ++attempt) {   // ride out Wi-Fi hiccups
        r = request(nam, "GET", base.resolved(QUrl(QStringLiteral("ScannerCapabilities"))), {},
                    15000);
        if (r.code == 200)
            break;
        QThread::sleep(3);
    }
    if (r.code != 200)
        return QCoreApplication::translate("jScan", "Network scanner is not responding (%1).")
            .arg(r.error.isEmpty() ? QStringLiteral("HTTP %1").arg(r.code) : r.error);

    bool platen = false, adf = false, duplex = false;
    QSet<QString> modes;
    QSet<int> res;
    double maxW = 0, maxH = 0, platW = 0, platH = 0;
    QStringList path;
    QXmlStreamReader xml(r.body);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString n = xml.name().toString();
            path << n;
            if (n == QLatin1String("PlatenInputCaps"))
                platen = true;
            else if (n == QLatin1String("AdfSimplexInputCaps"))
                adf = true;
            else if (n == QLatin1String("AdfDuplexInputCaps"))
                adf = duplex = true;
        } else if (xml.isEndElement()) {
            if (!path.isEmpty())
                path.removeLast();
        } else if (xml.isCharacters() && !xml.isWhitespace() && !path.isEmpty()) {
            const QString n = path.last();
            const QString t = xml.text().toString().trimmed();
            if (n == QLatin1String("ColorMode"))
                modes << t;
            else if (n == QLatin1String("XResolution") && path.contains(QLatin1String("DiscreteResolution")))
                res << t.toInt();
            else if (n == QLatin1String("MaxWidth") || n == QLatin1String("MaxHeight")) {
                const double mm = t.toDouble() / 300.0 * 25.4;
                const bool w = n == QLatin1String("MaxWidth");
                (w ? maxW : maxH) = std::max(w ? maxW : maxH, mm);
                if (path.contains(QLatin1String("PlatenInputCaps")))
                    (w ? platW : platH) = std::max(w ? platW : platH, mm);
            }
        }
    }

    caps->device = deviceName;
    caps->sources.clear();
    if (adf)
        caps->sources << QStringLiteral("ADF");
    if (duplex)
        caps->sources << QStringLiteral("ADF Duplex");
    if (platen)
        caps->sources << QStringLiteral("Flatbed");
    caps->modes.clear();
    if (modes.contains(QStringLiteral("RGB24")))
        caps->modes << QStringLiteral("Color");
    if (modes.contains(QStringLiteral("Grayscale8")))
        caps->modes << QStringLiteral("Gray") << QStringLiteral("Lineart");
    if (caps->modes.isEmpty())
        caps->modes << QStringLiteral("Color");
    QList<int> rl = res.values();
    std::sort(rl.begin(), rl.end());
    rl.removeAll(0);
    caps->resolutions = rl.isEmpty() ? QList<int>{150, 300} : rl;
    caps->maxMm = QSizeF(maxW, maxH);
    caps->flatbedMm = QSizeF(platW, platH);
    return {};
}

int scan(const ScanSettings &s, const DeviceCaps &caps, const std::atomic_bool &cancel,
         const Callbacks &cb, QString *error)
{
    QNetworkAccessManager nam;
    const QUrl base = baseUrl(s.device);
    const bool feeder = ScanWorker::isFeeder(s.source);

    int dpi = s.preview ? (caps.resolutions.isEmpty() ? 75 : caps.resolutions.first()) : s.dpi;
    if (!caps.resolutions.isEmpty() && !caps.resolutions.contains(dpi)) {
        int best = caps.resolutions.first();
        for (int r : caps.resolutions)
            if (std::abs(r - dpi) < std::abs(best - dpi))
                best = r;
        dpi = best;
    }

    const QSizeF limit = !feeder && !caps.flatbedMm.isEmpty() ? caps.flatbedMm : caps.maxMm;
    QRectF area(QPointF(0, 0), s.paperMm);
    if (s.preview)
        area = QRectF(QPointF(0, 0), limit);
    else if (!feeder && !s.areaMm.isEmpty())
        area = s.areaMm;
    if (!limit.isEmpty())
        area = area.intersected(QRectF(QPointF(0, 0), limit));

    const QString mode = s.preview ? QStringLiteral("Color") : s.mode;
    const QByteArray job =
        QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<scan:ScanSettings xmlns:scan=\"http://schemas.hp.com/imaging/escl/2011/05/03\" "
            "xmlns:pwg=\"http://www.pwg.org/schemas/2010/12/sm\">\n"
            "  <pwg:Version>2.6</pwg:Version>\n"
            "  <pwg:ScanRegions><pwg:ScanRegion>"
            "<pwg:ContentRegionUnits>escl:ThreeHundredthsOfInches</pwg:ContentRegionUnits>"
            "<pwg:XOffset>%1</pwg:XOffset><pwg:YOffset>%2</pwg:YOffset>"
            "<pwg:Width>%3</pwg:Width><pwg:Height>%4</pwg:Height>"
            "</pwg:ScanRegion></pwg:ScanRegions>\n"
            "  <pwg:InputSource>%5</pwg:InputSource>\n"
            "%6"
            "  <scan:ColorMode>%7</scan:ColorMode>\n"
            "  <scan:XResolution>%8</scan:XResolution><scan:YResolution>%8</scan:YResolution>\n"
            "  <pwg:DocumentFormat>image/jpeg</pwg:DocumentFormat>\n"
            "  <scan:DocumentFormatExt>image/jpeg</scan:DocumentFormatExt>\n"
            "</scan:ScanSettings>\n")
            .arg(toEsclUnits(area.left()))
            .arg(toEsclUnits(area.top()))
            .arg(toEsclUnits(area.width()))
            .arg(toEsclUnits(area.height()))
            .arg(feeder ? QStringLiteral("Feeder") : QStringLiteral("Platen"))
            .arg(s.source == QLatin1String("ADF Duplex")
                     ? QStringLiteral("  <scan:Duplex>true</scan:Duplex>\n")
                     : QString())
            .arg(modeToEscl(mode))
            .arg(dpi)
            .toUtf8();

    // --- create the job (printer may be waking up / briefly offline) ---------
    cb.status(QCoreApplication::translate("jScan", "Sending job to network scanner …"));
    QUrl jobUrl;
    QElapsedTimer t;
    t.start();
    while (!cancel) {
        Reply r = request(nam, "POST", base.resolved(QUrl(QStringLiteral("ScanJobs"))), job);
        if ((r.code == 201 || r.code == 200) && r.location.isValid()) {
            jobUrl = r.location;
            break;
        }
        if (r.code == 409 && feeder) {   // conflict: usually empty feeder
            *error = QCoreApplication::translate("jScan", "No sheets in the feeder.");
            return 0;
        }
        if (r.code != 0 && r.code != 503) {
            *error = QCoreApplication::translate("jScan", "The scanner rejected the job (HTTP %1).").arg(r.code);
            return 0;
        }
        if (t.elapsed() > 60000) {
            *error = QCoreApplication::translate("jScan", "Network scanner not reachable%1.")
                         .arg(r.error.isEmpty() ? QString() : QStringLiteral(": ") + r.error);
            return 0;
        }
        cb.status(QCoreApplication::translate("jScan", "Waiting for the scanner …"));
        sleepMs(2000, cancel);
    }
    if (cancel) {
        *error = QCoreApplication::translate("jScan", "Cancelled.");
        return 0;
    }

    // --- fetch pages until 404 ------------------------------------------------
    int pages = 0;
    const QUrl next = QUrl(jobUrl.toString() + QStringLiteral("/NextDocument"));
    t.restart();
    while (!cancel) {
        cb.status(s.preview ? QCoreApplication::translate("jScan", "Quick scan …")
                  : feeder  ? QCoreApplication::translate("jScan", "Scanning page %1 …").arg(pages + 1)
                            : QCoreApplication::translate("jScan", "Scanning document …"));
        Reply r = request(nam, "GET", next, {}, 120000);
        if (r.code == 200) {
            QImage img = QImage::fromData(r.body);
            if (img.isNull()) {
                *error = QCoreApplication::translate("jScan", "Cannot read the image from the scanner.");
                break;
            }
            const int dpm = int(std::lround(dpi / 0.0254));
            img.setDotsPerMeterX(dpm);
            img.setDotsPerMeterY(dpm);
            if (mode == QLatin1String("Lineart"))
                img = toLineart(img);
            else if (mode == QLatin1String("Gray"))
                img = img.convertToFormat(QImage::Format_Grayscale8);
            else
                img = img.convertToFormat(QImage::Format_RGB888);
            ++pages;
            cb.page(img);
            t.restart();
            continue;
        }
        if (r.code == 404 || r.code == 410)   // no more pages
            break;
        // 503 = not ready yet, 0 = network hiccup: keep trying for a while
        if (t.elapsed() > 120000) {
            *error = QCoreApplication::translate("jScan", "Timed out waiting for the scanner.");
            break;
        }
        sleepMs(1000, cancel);
    }

    if (cancel) {
        request(nam, "DELETE", jobUrl, {}, 5000);
        *error = QCoreApplication::translate("jScan", "Cancelled.");
    } else if (pages == 0 && error->isEmpty()) {
        *error = feeder ? QCoreApplication::translate("jScan", "No sheets in the feeder.")
                        : QCoreApplication::translate("jScan", "The scanner did not deliver an image.");
    }
    return pages;
}

} // namespace Escl
