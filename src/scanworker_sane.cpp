// SANE implementation of ScanWorker (Linux, BSD, macOS).
#include "scanworker.h"

#include <QCoreApplication>
#include "esclclient.h"
#include "v600scanner.h"
#include "compat.h"

#include <sane/sane.h>
#include <sane/saneopts.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

QString statusText(SANE_Status s)
{
    switch (s) {
    case SANE_STATUS_NO_DOCS:     return QCoreApplication::translate("jScan", "No sheets in the feeder.");
    case SANE_STATUS_JAMMED:      return QCoreApplication::translate("jScan", "Paper jam – please check the feeder.");
    case SANE_STATUS_COVER_OPEN:  return QCoreApplication::translate("jScan", "The cover is open.");
    case SANE_STATUS_DEVICE_BUSY: return QCoreApplication::translate("jScan", "The scanner is busy.");
    case SANE_STATUS_ACCESS_DENIED:
        return QCoreApplication::translate("jScan", "No access to the scanner (permissions?).");
    case SANE_STATUS_IO_ERROR:    return QCoreApplication::translate("jScan", "I/O error at the scanner.");
    default:                      return QString::fromUtf8(sane_strstatus(s));
    }
}

// SANE is initialised once per process and deliberately never shut down:
// sane_exit() unloads the backend libraries, and network backends (escl,
// airscan) may still have discovery threads running inside them, which then
// crash. Replugging still works because sane_get_devices()/sane_open() probe
// the hardware again on every call.
struct SaneSession {
    bool ok = false;
    SaneSession()
    {
        static const bool initialised = [] {
            SANE_Int v = 0;
            return sane_init(&v, nullptr) == SANE_STATUS_GOOD;
        }();
        ok = initialised;
    }
};

class Device
{
public:
    ~Device() { close(); }

    SANE_Status open(const QString &name)
    {
        const QByteArray n = name.toUtf8();
        SANE_Status st = sane_open(n.constData(), &m_h);
        // USB bus/device numbers change on replug: fall back to the backend's
        // first device ("canon_dr" alone opens exactly that).
        if (st != SANE_STATUS_GOOD) {
            const int colon = name.indexOf(QLatin1Char(':'));
            if (colon > 0 && name.contains(QLatin1String("usb"))) {
                const QByteArray backend = name.left(colon).toUtf8();
                st = sane_open(backend.constData(), &m_h);
            }
        }
        if (st != SANE_STATUS_GOOD)
            m_h = nullptr;
        return st;
    }

    void close()
    {
        if (m_h) {
            sane_cancel(m_h);
            sane_close(m_h);
            m_h = nullptr;
        }
    }

    SANE_Handle handle() const { return m_h; }

    const SANE_Option_Descriptor *desc(const char *name, int *index = nullptr) const
    {
        for (int i = 1;; ++i) {
            const SANE_Option_Descriptor *d = sane_get_option_descriptor(m_h, i);
            if (!d)
                return nullptr;
            if (d->name && std::strcmp(d->name, name) == 0) {
                if (index)
                    *index = i;
                return d;
            }
        }
    }

    QStringList stringList(const char *name) const
    {
        QStringList out;
        const SANE_Option_Descriptor *d = desc(name);
        if (d && d->type == SANE_TYPE_STRING
            && d->constraint_type == SANE_CONSTRAINT_STRING_LIST) {
            for (const SANE_String_Const *s = d->constraint.string_list; *s; ++s)
                out << QString::fromUtf8(*s);
        }
        return out;
    }

    QList<int> resolutions() const
    {
        QList<int> out;
        const SANE_Option_Descriptor *d = desc(SANE_NAME_SCAN_RESOLUTION);
        if (!d)
            return out;
        const bool fixed = d->type == SANE_TYPE_FIXED;
        auto val = [fixed](SANE_Word w) { return fixed ? int(SANE_UNFIX(w)) : int(w); };
        if (d->constraint_type == SANE_CONSTRAINT_WORD_LIST) {
            for (int k = 1; k <= d->constraint.word_list[0]; ++k)
                out << val(d->constraint.word_list[k]);
        } else if (d->constraint_type == SANE_CONSTRAINT_RANGE) {
            const int lo = val(d->constraint.range->min), hi = val(d->constraint.range->max);
            for (int r : {75, 100, 150, 200, 300, 400, 600, 1200, 2400, 4800, 6400})
                if (r >= lo && r <= hi)
                    out << r;
        }
        return out;
    }

    double rangeMax(const char *name) const
    {
        const SANE_Option_Descriptor *d = desc(name);
        if (!d || d->constraint_type != SANE_CONSTRAINT_RANGE)
            return 0;
        return d->type == SANE_TYPE_FIXED ? SANE_UNFIX(d->constraint.range->max)
                                          : double(d->constraint.range->max);
    }

    bool setString(const char *name, const QString &value)
    {
        int i = 0;
        const SANE_Option_Descriptor *d = desc(name, &i);
        if (!d || !SANE_OPTION_IS_ACTIVE(d->cap) || d->type != SANE_TYPE_STRING)
            return false;
        QByteArray buf(d->size, '\0');
        const QByteArray v = value.toUtf8();
        std::memcpy(buf.data(), v.constData(), std::min<int>(v.size(), d->size - 1));
        return sane_control_option(m_h, i, SANE_ACTION_SET_VALUE, buf.data(), nullptr)
               == SANE_STATUS_GOOD;
    }

    bool setBool(const char *name, bool value)
    {
        int i = 0;
        const SANE_Option_Descriptor *d = desc(name, &i);
        if (!d || !SANE_OPTION_IS_ACTIVE(d->cap) || d->type != SANE_TYPE_BOOL)
            return false;
        SANE_Bool b = value ? SANE_TRUE : SANE_FALSE;
        return sane_control_option(m_h, i, SANE_ACTION_SET_VALUE, &b, nullptr)
               == SANE_STATUS_GOOD;
    }

    // Sets an INT or FIXED option, snapping to the option's constraint.
    bool setNumber(const char *name, double value)
    {
        int i = 0;
        const SANE_Option_Descriptor *d = desc(name, &i);
        if (!d || !SANE_OPTION_IS_ACTIVE(d->cap))
            return false;
        const bool fixed = d->type == SANE_TYPE_FIXED;
        if (!fixed && d->type != SANE_TYPE_INT)
            return false;
        auto toDouble = [fixed](SANE_Word w) { return fixed ? SANE_UNFIX(w) : double(w); };

        if (d->constraint_type == SANE_CONSTRAINT_RANGE) {
            value = std::clamp(value, toDouble(d->constraint.range->min),
                               toDouble(d->constraint.range->max));
        } else if (d->constraint_type == SANE_CONSTRAINT_WORD_LIST) {
            const SANE_Word *list = d->constraint.word_list;
            double best = value, bestDiff = 1e300;
            for (int k = 1; k <= list[0]; ++k) {
                const double cand = toDouble(list[k]);
                if (std::abs(cand - value) < bestDiff) {
                    bestDiff = std::abs(cand - value);
                    best = cand;
                }
            }
            value = best;
        }
        SANE_Word w = fixed ? SANE_FIX(value) : SANE_Word(std::lround(value));
        return sane_control_option(m_h, i, SANE_ACTION_SET_VALUE, &w, nullptr)
               == SANE_STATUS_GOOD;
    }

private:
    SANE_Handle m_h = nullptr;
};

QImage toImage(const QByteArray &data, const SANE_Parameters &p, int dpi)
{
    if (p.bytes_per_line <= 0 || p.pixels_per_line <= 0)
        return {};
    const int lines = int(data.size() / p.bytes_per_line);
    if (lines <= 0)
        return {};

    QImage::Format fmt;
    if (p.format == SANE_FRAME_RGB && p.depth == 8)
        fmt = QImage::Format_RGB888;
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    else if (p.format == SANE_FRAME_RGB && p.depth == 16)
        fmt = QImage::Format_RGBX64;
#endif
    else if (p.format == SANE_FRAME_GRAY && p.depth == 8)
        fmt = QImage::Format_Grayscale8;
#if QT_VERSION >= QT_VERSION_CHECK(5, 13, 0)
    else if (p.format == SANE_FRAME_GRAY && p.depth == 16)
        fmt = QImage::Format_Grayscale16;
#endif
    else if (p.format == SANE_FRAME_GRAY && p.depth == 1)
        fmt = QImage::Format_Mono;
    else
        return {};

    QImage img(p.pixels_per_line, lines, fmt);
    if (fmt == QImage::Format_Mono)   // SANE lineart: bit 1 = black
        img.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});

    for (int y = 0; y < lines; ++y) {
        const uchar *src = reinterpret_cast<const uchar *>(data.constData())
                           + qsizetype(y) * p.bytes_per_line;
        uchar *dst = img.scanLine(y);
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
        if (fmt == QImage::Format_RGBX64) {
            // SANE delivers 16 bit samples in host byte order; expand RGB -> RGBX.
            auto *s16 = reinterpret_cast<const quint16 *>(src);
            auto *d16 = reinterpret_cast<quint16 *>(dst);
            for (int x = 0; x < p.pixels_per_line; ++x) {
                d16[4 * x + 0] = s16[3 * x + 0];
                d16[4 * x + 1] = s16[3 * x + 1];
                d16[4 * x + 2] = s16[3 * x + 2];
                d16[4 * x + 3] = 0xffff;
            }
        } else
#endif
        {
            std::memcpy(dst, src, std::min<int>(p.bytes_per_line, int(img.bytesPerLine())));
        }
    }

    const int dpm = int(std::lround(dpi / 0.0254));
    img.setDotsPerMeterX(dpm);
    img.setDotsPerMeterY(dpm);
    return img;
}

QString connectionOf(const QString &name)
{
    if (name.contains(QLatin1String("usb"), Qt::CaseInsensitive))
        return QStringLiteral("USB");
    if (name.startsWith(QLatin1String("escl:")) || name.startsWith(QLatin1String("airscan:"))
        || name.contains(QLatin1String("net"), Qt::CaseInsensitive)
        || name.contains(QLatin1String("ip="))) {
        return QCoreApplication::translate("jScan", "Network");
    }
    return {};
}

} // namespace

void ScanWorker::listDevices()
{
    emit status(QCoreApplication::translate("jScan", "Searching for scanners (USB and network) …"));
    QList<ScanDevice> out;
    SaneSession session;
    const SANE_Device **list = nullptr;
    if (session.ok && sane_get_devices(&list, SANE_FALSE) == SANE_STATUS_GOOD && list) {
        QList<const SANE_Device *> all;
        for (int i = 0; list[i]; ++i)
            all << list[i];

        // Network devices often show up through several drivers. Keep only
        // the best one per device: escl (handled by our own patient eSCL
        // client) > airscan > hpaio.
        auto rank = [](const QString &n) {
            if (n.startsWith(QLatin1String("escl:")))    return 3;
            if (n.startsWith(QLatin1String("airscan:"))) return 2;
            if (n.startsWith(QLatin1String("hpaio:")))   return 1;
            return 0;   // local backends: always shown
        };
        QList<ScanDevice> cand;
        QList<int> ranks;
        const bool ownV600 = V600::isPresent();
        for (const SANE_Device *d : all) {
            const QString n = QString::fromUtf8(d->name);
            if (n.startsWith(QLatin1String("v4l:")))   // webcams
                continue;
            // Epson V600 via Epson's closed plugin: our own driver replaces it
            if (ownV600 && n.startsWith(QLatin1String("epkowa:"))
                && QString::fromUtf8(d->model).contains(QLatin1String("V600")))
                continue;
            ScanDevice dev;
            dev.name = n;
            QString model = QString::fromUtf8(d->model).simplified();
            model.remove(QRegularExpression(QStringLiteral(R"(\s*\[[0-9A-Fa-f]+\]$)")));
            QString vendor = QString::fromUtf8(d->vendor).simplified();
            if (vendor.compare(QLatin1String("Hewlett-Packard"), Qt::CaseInsensitive) == 0)
                vendor = QStringLiteral("HP");
            if (model.startsWith(vendor, Qt::CaseInsensitive))
                vendor.clear();
            dev.key = (vendor + QLatin1Char(' ') + model).trimmed();
            dev.label = dev.key;
            const QString conn = connectionOf(n);
            if (!conn.isEmpty())
                dev.label += QStringLiteral(" (%1)").arg(conn);
            cand << dev;
            ranks << rank(n);
        }
        for (int i = 0; i < cand.size(); ++i) {
            bool beaten = false;
            if (ranks[i] > 0) {
                for (int j = 0; j < cand.size() && !beaten; ++j) {
                    if (j == i || ranks[j] == 0 || cand[j].key != cand[i].key)
                        continue;
                    // a better driver, or the same driver listed earlier
                    beaten = ranks[j] > ranks[i] || (ranks[j] == ranks[i] && j < i);
                }
            }
            if (!beaten)
                out << cand[i];
        }
    }
    if (V600::isPresent()) {
        ScanDevice dev;
        dev.name = V600::deviceName();
        dev.key = QStringLiteral("Epson Perfection V600 Photo");
        dev.label = dev.key + QStringLiteral(" (USB)");
        out.prepend(dev);
    }
    emit devicesFound(out);
}

void ScanWorker::queryCaps(const QString &device)
{
    emit status(QCoreApplication::translate("jScan", "Querying scanner capabilities …"));
    DeviceCaps caps;
    caps.device = device;
    if (Escl::handles(device)) {
        const QString err = Escl::queryCaps(device, &caps);
        if (err.isEmpty())
            m_lastCaps = caps;
        emit capsReady(caps, err);
        return;
    }
    if (V600::handles(device)) {
        // Tell the user once per run if the firmware file is missing, even
        // if the scanner still has it loaded (it is lost when switched off).
        static bool firmwareNoticeShown = false;
        if (!firmwareNoticeShown && V600::firmwarePath().isEmpty()) {
            firmwareNoticeShown = true;
            emit notice(V600::firmwareMissingTitle(), V600::firmwareMissingText());
        }
        caps.sources = QStringList{QStringLiteral("Flatbed")};
        caps.modes = QStringList{QStringLiteral("Color"), QStringLiteral("Gray")};
        if (V600::supports16Bit())
            caps.modes << QStringLiteral("Color16") << QStringLiteral("Gray16");
        caps.resolutions = V600::resolutions();
        caps.maxMm = V600::glassMm();
        m_lastCaps = caps;
        emit capsReady(caps, {});
        return;
    }
    SaneSession session;
    Device dev;
    const SANE_Status st = session.ok ? dev.open(device) : SANE_STATUS_INVAL;
    if (st != SANE_STATUS_GOOD) {
        emit capsReady(caps, QCoreApplication::translate("jScan", "Cannot open the scanner: %1")
                                 .arg(statusText(st)));
        return;
    }
    caps.sources = dev.stringList(SANE_NAME_SCAN_SOURCE);
    caps.modes = dev.stringList(SANE_NAME_SCAN_MODE);
    caps.resolutions = dev.resolutions();
    double w = dev.rangeMax("page-width");
    double h = dev.rangeMax("page-height");
    if (w <= 0)
        w = dev.rangeMax(SANE_NAME_SCAN_BR_X);
    if (h <= 0)
        h = dev.rangeMax(SANE_NAME_SCAN_BR_Y);
    caps.maxMm = QSizeF(w, h);
    emit capsReady(caps, {});
}

void ScanWorker::scan(const ScanSettings &s)
{
    m_cancel = false;
    int pages = 0;

    if (Escl::handles(s.device)) {
        if (m_lastCaps.device != s.device)
            Escl::queryCaps(s.device, &m_lastCaps);
        QString error;
        Escl::Callbacks cb;
        cb.status = [this](const QString &t) { emit status(t); };
        cb.page = [this, &s](const QImage &img) {
            if (s.preview)
                emit previewReady(img);
            else
                emit pageScanned(img);
        };
        pages = Escl::scan(s, m_lastCaps, m_cancel, cb, &error);
        emit finished(pages, error);
        return;
    }

    if (V600::handles(s.device)) {
        V600::Callbacks cb;
        cb.status = [this](const QString &t) { emit status(t); };
        cb.progress = [this](const QImage &part, int expectedHeight) {
            emit pageProgress(part, expectedHeight);
        };
        QRectF area(QPointF(0, 0), s.paperMm);
        if (s.preview)
            area = QRectF(QPointF(0, 0), V600::glassMm());
        else if (!s.areaMm.isEmpty())
            area = s.areaMm;
        if (s.preview)
            emit status(QCoreApplication::translate("jScan", "Quick scan …"));
        QString error;
        const bool gray = !s.preview && s.mode.startsWith(QLatin1String("Gray"));
        const int depth = !s.preview && s.mode.endsWith(QLatin1String("16")) ? 16 : 8;
        const QImage img = V600::scan(area, gray, s.preview ? 100 : s.dpi, depth, m_cancel, cb, &error);
        if (img.isNull()) {
            if (error == V600::firmwareMissingError())
                emit notice(V600::firmwareMissingTitle(), V600::firmwareMissingText());
            emit finished(0, error);
            return;
        }
        if (s.preview)
            emit previewReady(img);
        else
            emit pageScanned(img);
        emit finished(1, {});
        return;
    }

    SaneSession session;
    if (!session.ok) {
        emit finished(0, QCoreApplication::translate("jScan", "Could not initialise SANE."));
        return;
    }

    emit status(QCoreApplication::translate("jScan", "Connecting to the scanner …"));
    Device dev;
    SANE_Status st = dev.open(s.device);
    if (st != SANE_STATUS_GOOD) {
        QString msg = QCoreApplication::translate("jScan", "Scanner not reachable: %1").arg(statusText(st));
        if (s.device.startsWith(QLatin1String("canon_dr")))
            msg += QStringLiteral("\n\n")
                   + QCoreApplication::translate("jScan", "Is it switched on and connected via USB, and is the "
                                                 "“Auto Start” switch on the back set to OFF?");
        emit finished(0, msg);
        return;
    }

    if (!s.source.isEmpty())
        dev.setString(SANE_NAME_SCAN_SOURCE, s.source);
    const bool feeder = isFeeder(s.source);
    const QString mode = s.preview ? QStringLiteral("Color") : s.mode;
    if (!mode.isEmpty())
        dev.setString(SANE_NAME_SCAN_MODE, mode);

    // The backend may snap the resolution: use what it actually accepted.
    int dpi = s.preview ? 75 : s.dpi;
    dev.setNumber(SANE_NAME_SCAN_RESOLUTION, dpi);
    if (const SANE_Option_Descriptor *d = dev.desc(SANE_NAME_SCAN_RESOLUTION)) {
        int idx = 0;
        dev.desc(SANE_NAME_SCAN_RESOLUTION, &idx);
        SANE_Word w = 0;
        if (sane_control_option(dev.handle(), idx, SANE_ACTION_GET_VALUE, &w, nullptr)
            == SANE_STATUS_GOOD)
            dpi = d->type == SANE_TYPE_FIXED ? int(SANE_UNFIX(w)) : int(w);
    }

    // canon_dr and other sheet-feed backends: tell them the paper size
    // (centres the scan area on the feeder guides).
    QRectF area(QPointF(0, 0), s.paperMm);
    if (s.preview)
        area = QRectF(0, 0, 10000, 10000);   // clamped to the whole glass
    else if (!feeder && !s.areaMm.isEmpty())
        area = s.areaMm;
    if (feeder) {
        dev.setNumber("page-width", s.paperMm.width());
        dev.setNumber("page-height", s.paperMm.height());
    }
    dev.setNumber(SANE_NAME_SCAN_TL_X, 0);
    dev.setNumber(SANE_NAME_SCAN_TL_Y, 0);
    dev.setNumber(SANE_NAME_SCAN_BR_X, area.right());
    dev.setNumber(SANE_NAME_SCAN_BR_Y, area.bottom());
    dev.setNumber(SANE_NAME_SCAN_TL_X, area.left());
    dev.setNumber(SANE_NAME_SCAN_TL_Y, area.top());
    dev.setBool("rollerdeskew", s.deskew);

    QString error;
    QByteArray buf(256 * 1024, Qt::Uninitialized);
    SANE_Handle h = dev.handle();

    while (!m_cancel) {
        emit status(s.preview ? QCoreApplication::translate("jScan", "Quick scan …")
                    : feeder  ? QCoreApplication::translate("jScan", "Scanning page %1 …").arg(pages + 1)
                              : QCoreApplication::translate("jScan", "Scanning document …"));
        st = sane_start(h);
        if (st == SANE_STATUS_NO_DOCS) {
            if (pages == 0)
                error = statusText(st);
            break;
        }
        if (st != SANE_STATUS_GOOD) {
            error = statusText(st);
            break;
        }

        SANE_Parameters params{};
        sane_get_parameters(h, &params);
        QByteArray data;
        if (params.lines > 0)
            data.reserve(qsizetype(params.lines) * params.bytes_per_line);

        QElapsedTimer liveTimer;
        liveTimer.start();
        for (;;) {
            if (m_cancel) {
                sane_cancel(h);
                break;
            }
            SANE_Int len = 0;
            st = sane_read(h, reinterpret_cast<SANE_Byte *>(buf.data()), int(buf.size()), &len);
            if (len > 0)
                data.append(buf.constData(), len);
            if (st != SANE_STATUS_GOOD)
                break;
            if (liveTimer.elapsed() > 150 && params.bytes_per_line > 0) {
                liveTimer.restart();
                const QImage part = toImage(data, params, dpi);
                if (!part.isNull()) {
                    const int w = std::min(part.width(), 900);
                    const double f = double(w) / part.width();
                    emit pageProgress(part.scaledToWidth(w, Qt::FastTransformation),
                                      params.lines > 0 ? int(params.lines * f) : 0);
                }
                if (params.lines > 0 && !s.preview) {
                    const int done = int(data.size() / params.bytes_per_line);
                    emit status((feeder ? QCoreApplication::translate("jScan", "Scanning page %1 … %2 %")
                                              .arg(pages + 1)
                                        : QCoreApplication::translate("jScan", "Scanning document … %1 %"))
                                    .arg(std::min(100, done * 100 / params.lines)));
                }
            }
        }
        if (m_cancel)
            break;
        if (st != SANE_STATUS_EOF) {
            error = statusText(st);
            break;
        }

        // Parameters may only be exact once the frame has been read.
        sane_get_parameters(h, &params);
        QImage img = toImage(data, params, dpi);
        if (img.isNull()) {
            error = QCoreApplication::translate("jScan", "The scanner's image format is not supported.");
            break;
        }
        ++pages;
        if (s.preview) {
            emit previewReady(img);
            break;
        }
        emit pageScanned(img);

        if (!feeder)   // flatbed: one page per click
            break;
    }

    dev.close();
    if (m_cancel && error.isEmpty())
        error = QCoreApplication::translate("jScan", "Cancelled.");
    emit finished(pages, error);
}
