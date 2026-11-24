// Open driver for the Epson Perfection V600 Photo, see v600scanner.h.
//
// Protocol (all little endian), as observed on the USB bus:
//  - commands are two bytes (1b xx / 1e xx); most are acknowledged with 06,
//    followed by a parameter block that is acknowledged with 06 again
//  - 1b 03 status: byte 0 bit 0x10 = firmware running, 0x02 = initialising,
//    0x40 = busy (carriage moving)
//  - firmware upload: 1b 06, 01 00 01 00, 64 KiB payload, 8-bit sum, 1b 16, 80
//  - a scan consists of calibration passes (dark level, white reference with
//    the lamp on, dark reference with the lamp off) and the final pass, each
//    set up with 1e 22 (exposure), 1e 41 (window), 1e 42 (levels), 1e 43,
//    started with 1e 47; data arrives in blocks of whole lines plus an 8 byte
//    trailer
//  - the CCD lines for red, green and blue are 20 / 10 / 0 lines apart at
//    400 dpi; the host aligns them
//  - the scanner applies the shading table (per pixel white level) and
//    delivers gamma-encoded 8-bit RGB; the host applies the colour matrix
#include "v600scanner.h"

#include "v600_tables_400.h"

#include <libusb-1.0/libusb.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kVid = 0x04b8;
constexpr uint16_t kPid = 0x013a;
constexpr unsigned char kEpOut = 0x02;
constexpr unsigned char kEpIn = 0x81;
constexpr unsigned kTimeoutMs = 5000;
constexpr unsigned char kAck = 0x06;

constexpr int kFwHeader = 256;
constexpr int kFwPayload = 65536;
const char kFwName[] = "esfwA1.bin";

constexpr double kGlassW = 215.9;
constexpr double kGlassH = 297.18;

// Colour correction matrix (the values Epson's driver uses by default).
const double kCct[3][3] = {{1.2578, -0.213989, -0.0437927},
                           {-0.193893, 1.2856, -0.0916901},
                           {-0.0257874, -0.264191, 1.28999}};

using Bytes = std::vector<unsigned char>;

void put16(Bytes &v, size_t o, unsigned x)
{
    v[o] = static_cast<unsigned char>(x & 0xff);
    v[o + 1] = static_cast<unsigned char>((x >> 8) & 0xff);
}

void put32(Bytes &v, size_t o, uint32_t x)
{
    for (size_t i = 0; i < 4; ++i)
        v[o + i] = static_cast<unsigned char>((x >> (8 * i)) & 0xff);
}

class Usb
{
public:
    ~Usb() { close(); }

    // 0 on success, otherwise a libusb error code.
    int open()
    {
        int r = libusb_init(&m_ctx);
        if (r != 0) {
            m_ctx = nullptr;
            return r;
        }
        libusb_device **list = nullptr;
        const ssize_t n = libusb_get_device_list(m_ctx, &list);
        r = LIBUSB_ERROR_NO_DEVICE;
        for (ssize_t i = 0; i < n; ++i) {
            libusb_device_descriptor d{};
            if (libusb_get_device_descriptor(list[i], &d) != 0 || d.idVendor != kVid
                || d.idProduct != kPid)
                continue;
            r = libusb_open(list[i], &m_h);
            break;
        }
        if (list)
            libusb_free_device_list(list, 1);
        if (r != 0) {
            m_h = nullptr;
            return r;
        }
        r = libusb_claim_interface(m_h, 0);
        if (r != 0) {
            libusb_close(m_h);
            m_h = nullptr;
            return r;
        }
        m_claimed = true;
        return 0;
    }

    void close()
    {
        if (m_h) {
            if (m_claimed)
                libusb_release_interface(m_h, 0);
            libusb_close(m_h);
            m_h = nullptr;
            m_claimed = false;
        }
        if (m_ctx) {
            libusb_exit(m_ctx);
            m_ctx = nullptr;
        }
    }

    QString lastError() const { return m_error; }

    bool write(const unsigned char *d, size_t n)
    {
        size_t done = 0;
        while (done < n) {
            int got = 0;
            const int r = libusb_bulk_transfer(m_h, kEpOut, const_cast<unsigned char *>(d + done),
                                               int(n - done), &got, kTimeoutMs);
            if (r != 0)
                return fail(r);
            done += size_t(got);
        }
        return true;
    }
    bool write(const Bytes &d) { return write(d.data(), d.size()); }

    bool read(unsigned char *d, size_t n)
    {
        size_t done = 0;
        while (done < n) {
            int got = 0;
            const int r = libusb_bulk_transfer(m_h, kEpIn, d + done, int(n - done), &got, kTimeoutMs);
            if (r != 0)
                return fail(r);
            done += size_t(got);
        }
        return true;
    }

    // One bulk IN transfer; returns the number of bytes or -1.
    int readChunk(unsigned char *d, size_t n, unsigned timeoutMs)
    {
        int got = 0;
        const int r = libusb_bulk_transfer(m_h, kEpIn, d, int(n), &got, timeoutMs);
        if (r != 0) {
            fail(r);
            return -1;
        }
        return got;
    }

    bool ack()
    {
        unsigned char a = 0;
        if (!read(&a, 1))
            return false;
        if (a != kAck) {
            m_error = QStringLiteral("unexpected reply %1").arg(a, 2, 16, QLatin1Char('0'));
            return false;
        }
        return true;
    }

    bool status(unsigned char st[2])
    {
        static const unsigned char q[2] = {0x1b, 0x03};
        return write(q, 2) && read(st, 2);
    }

    // 1e xx [+ params], each acknowledged
    bool cmd(unsigned char c) { return write({0x1e, c}) && ack(); }
    bool cmd(unsigned char c, const Bytes &p) { return cmd(c) && write(p) && ack(); }

    bool table(const Bytes &hdr, const unsigned char *d, size_t n)
    {
        return cmd(0x84) && write(hdr) && write(d, n) && ack();
    }

    // Waits until the scanner is neither initialising nor busy.
    bool waitIdle(int seconds = 60)
    {
        unsigned char st[2];
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (!status(st))
                return false;
            if (!(st[0] & 0x42))
                return true;
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(seconds)) {
                m_error = QStringLiteral("timeout, status %1 %2")
                              .arg(st[0], 2, 16, QLatin1Char('0'))
                              .arg(st[1], 2, 16, QLatin1Char('0'));
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 1e 47 starts a pass (no ACK); reads `total` bytes.
    bool startAndRead(Bytes &data, size_t total)
    {
        if (!write({0x1e, 0x47}))
            return false;
        data.resize(total);
        size_t done = 0;
        while (done < total) {
            const int got = readChunk(data.data() + done, std::min<size_t>(51200, total - done), 30000);
            if (got <= 0)
                return got < 0 ? false : fail(LIBUSB_ERROR_IO);
            done += size_t(got);
        }
        return true;
    }

private:
    bool fail(int r)
    {
        m_error = QString::fromLatin1(libusb_error_name(r));
        return false;
    }

    libusb_context *m_ctx = nullptr;
    libusb_device_handle *m_h = nullptr;
    bool m_claimed = false;
    QString m_error;
};

Bytes window41(unsigned start, unsigned end, uint32_t lines, std::initializer_list<unsigned char> rest)
{
    Bytes v(22, 0);
    put16(v, 0, start);
    put16(v, 2, end);
    put32(v, 4, lines);
    size_t o = 8;
    for (unsigned char b : rest)
        v[o++] = b;
    return v;
}

// Mean per channel of interleaved 16-bit RGB samples.
void channelMeans(const unsigned char *d, size_t samples, double out[3])
{
    double sum[3] = {0, 0, 0};
    for (size_t i = 0; i < samples; ++i)
        sum[i % 3] += d[2 * i] | (d[2 * i + 1] << 8);
    for (int c = 0; c < 3; ++c)
        out[c] = sum[c] * 3.0 / double(samples);
}

bool loadFirmware(Bytes *fw, QString *error)
{
    const QString path = V600::firmwarePath();
    if (path.isEmpty()) {
        *error = V600::firmwareMissingError();
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = QCoreApplication::translate("jScan", "Cannot read the firmware file %1.").arg(path);
        return false;
    }
    const QByteArray data = f.readAll();
    if (data.size() < kFwHeader + kFwPayload || !data.startsWith("EDLA")) {
        *error = QCoreApplication::translate("jScan", "%1 is not a valid firmware file for the Epson Perfection V600.").arg(path);
        return false;
    }
    fw->assign(data.constData() + kFwHeader, data.constData() + kFwHeader + kFwPayload);
    return true;
}

bool uploadFirmware(Usb &usb, const Bytes &fw)
{
    unsigned char sum = 0;
    for (unsigned char b : fw)
        sum = static_cast<unsigned char>(sum + b);
    return usb.write({0x1b, 0x06}) && usb.ack() && usb.write({0x01, 0x00, 0x01, 0x00})
           && usb.write(fw) && usb.write(&sum, 1) && usb.ack() && usb.write({0x1b, 0x16})
           && usb.ack() && usb.write({0x80}) && usb.ack();
}

QString openError(int r)
{
    switch (r) {
    case LIBUSB_ERROR_NO_DEVICE:
    case LIBUSB_ERROR_NOT_FOUND:
        return QCoreApplication::translate("jScan", "Epson Perfection V600 not found. Is it switched on and connected via USB?");
    case LIBUSB_ERROR_ACCESS:
        return QCoreApplication::translate("jScan", "No access to the scanner (permissions?).") + QStringLiteral("\n\n")
               + QCoreApplication::translate("jScan", "A udev rule is needed for USB ID 04b8:013a, see the README.");
    case LIBUSB_ERROR_BUSY:
        return QCoreApplication::translate("jScan", "The scanner is busy.");
    default:
        return QCoreApplication::translate("jScan", "Cannot open the scanner: %1").arg(QString::fromLatin1(libusb_error_name(r)));
    }
}

// Blocks of the final pass, read in a thread of their own so the scanner
// never has to wait for the host (it would stop and back up the carriage).
struct BlockQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<Bytes> blocks;
    bool done = false;
    bool failed = false;
};

// Per optical resolution: sensor mode, timing, calibration and motor values
// as Epson's driver uses them (from USB captures).
struct Mode {
    int dpi;
    unsigned char sensorMode;   // 1e 11
    unsigned lineClock;         // 1e 21 (3200: for pass A; the scan clock depends on the width)
    unsigned char divider;      // 1e 41 byte 10
    unsigned char rows;         // 1e 41 byte 9: 2 = both staggered CCD rows (3200 dpi)
    unsigned char flag21;       // 1e 41 byte 21 / 1e 22 byte 10
    bool storedCalibration;     // 400 dpi: gains/offsets/exposure from 1e 93
    unsigned exposure;          // otherwise these (Epson's auto-calibration
    unsigned char gain;         // results, stable over many runs)
    unsigned char offset[3];
    unsigned colourOffset;      // lines between the R and B CCD rows
    // Matrix CCD: above 1600 dpi the pixels of a line come from staggered
    // sensor rows; pixel x is read phaseLines[x % phases] lines later.
    unsigned phases;
    unsigned phaseLines[4];
    // horizontal position of the phases within a group of `phases` pixels,
    // in pixels (measured; the host resamples them to an even grid)
    double phasePos[4];
    unsigned extraLines;        // lines read beyond height + colour offset
    // The phase -> sensor row assignment depends on the window start: keep
    // start % startMod == startRes (the values the line offsets were measured with).
    unsigned startMod, startRes;
    unsigned glassStart;        // sensor position of the glass edge (x units)
    unsigned xUnitDpi;          // window start is given in these units
    unsigned darkStart, darkEnd;   // masked sensor pixels (pass B)
    unsigned fullStart, fullEnd;   // whole sensor line (pass A)
    int prefeedOffset;          // prefeed (1600 dpi steps) = y + this, long ramp
    bool rampScan;              // final pass with the acceleration table (400 dpi)
    unsigned char runout;       // extra steps after the last line
    // above 1600 dpi: line clock of the final pass for a 1264 px window
    // (grows with the width), CCD timing details, final pass flags
    unsigned scanClock1264;
    unsigned darkEdge;          // timing edge for the dark pixel pass
    unsigned passAPeriod;       // timing period of pass A (0 = 2 * clock - 8)
    unsigned char final11;      // 1e 41 byte 11 of the final pass
    unsigned char final15;      // bits or'ed into 1e 41 byte 15
};

const Mode kModes[] = {
    {400, 0x85, 0x0528, 4, 1, 1, true, 0, 0, {0, 0, 0}, 20, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, 0, 1, 0, 103, 400, 0x10, 0x20,
     0x10, 0xdb0, -376, true, 0x80, 0, 0, 0, 0x83, 0},
    {800, 0x83, 0x12a0, 2, 1, 1, false, 0x09df, 0x8c, {0x2a, 0x1b, 0x21}, 40, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, 0, 1, 0, 207,
     800, 0x20, 0x30, 0xd7, 0x1b67, 136, false, 0x80, 0, 0, 0, 0x83, 0},
    {1600, 0x82, 0x2820, 1, 1, 1, false, 0x1410, 0x88, {0x2a, 0x1b, 0x20}, 80, 1, {0, 0, 0, 0}, {0, 0, 0, 0}, 0, 1, 0, 414,
     1600, 0x3d, 0x6d, 0x1af, 0x36cf, 232, false, 0x20, 0, 0, 0, 0x83, 0},
    {3200, 0x01, 0x3900, 1, 2, 0, false, 0x15b8, 0x88, {0x28, 0x17, 0x1d}, 160, 2, {0, 8, 0, 0}, {0, 1, 0, 0}, 9, 2, 0,
     460, 1600, 0x64, 0xc4, 0x1e2, 0x6c22, 232, false, 0x20, 7760, 676, 0, 0x83, 0},
    {6400, 0x00, 0x7140, 1, 4, 0, false, 0x15b0, 0x88, {0x28, 0x17, 0x1d}, 320, 4, {80, 16, 96, 0}, {0, 0.8, 2.1, 2.58},
     101, 4, 3, 490, 1600, 0xc8, 0x188, 0x214, 0xd694, 232, false, 0x20, 7952, 872, 58040, 0x80, 0x01},
};

// CCD timing table (1e 84 type 07): edges (time, signals), 0xffff filled.
// `start`/`end` is the pixel window; small = only the masked dark pixels.
Bytes timingTable(unsigned period, unsigned start, unsigned end, unsigned darkEdge)
{
    std::vector<unsigned> e = {0, 0, 40, 0x31c0, 200, 0x31c0};
    if (darkEdge)
        e.insert(e.end(), {400, 0x1000, darkEdge, 0x2000});
    else
        e.insert(e.end(), {400, 0x2000, (start + 288) / 8 * 8, 0x1000, end + 480, 0x2000});
    e.insert(e.end(), {period, 0x1000});
    Bytes t(256, 0xff);
    for (size_t i = 0; i < e.size(); ++i)
        put16(t, 2 * i, e[i]);
    return t;
}

const Mode &modeFor(int dpi)
{
    for (const Mode &m : kModes)
        if (dpi <= m.dpi)
            return m;
    return kModes[sizeof kModes / sizeof kModes[0] - 1];
}

} // namespace

namespace V600 {

QString deviceName()
{
    return QStringLiteral("v600:usb");
}

bool handles(const QString &device)
{
    return device.startsWith(QLatin1String("v600:"));
}

bool isPresent()
{
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != 0)
        return false;
    bool found = false;
    libusb_device **list = nullptr;
    const ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n && !found; ++i) {
        libusb_device_descriptor d{};
        found = libusb_get_device_descriptor(list[i], &d) == 0 && d.idVendor == kVid
                && d.idProduct == kPid;
    }
    if (list)
        libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return found;
}

// Directory name of the app's data: "jscan" (desktop), "harbour-jscan" (Sailfish OS).
QString appDirName()
{
    const QString n = QCoreApplication::applicationName();
    return n.isEmpty() ? QStringLiteral("jscan") : n;
}

QString perUserFirmwarePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QLatin1Char('/')
           + appDirName() + QStringLiteral("/firmware/") + QString::fromLatin1(kFwName);
}

QStringList firmwareSearchPaths()
{
    const QString name = QString::fromLatin1(kFwName);
    QStringList out;
    const QString env = QString::fromLocal8Bit(qgetenv("JSCAN_V600_FIRMWARE"));
    if (!env.isEmpty())
        out << env;
    // per user: ~/.local/share/<app>/firmware/esfwA1.bin
    out << perUserFirmwarePath();
    // earlier versions of the Sailfish app were called "jscan"
    if (appDirName() != QLatin1String("jscan"))
        out << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                   + QStringLiteral("/jscan/firmware/") + name;
    // system wide
    out << QStringLiteral("/usr/share/") + appDirName() + QStringLiteral("/firmware/") + name;
    // where Epson's own package (iscan-plugin-gt-x820) installs it
    out << QStringLiteral("/usr/share/iscan/") + name
        << QStringLiteral("/usr/local/share/iscan/") + name;
    return out;
}

QString firmwarePath()
{
    for (const QString &p : firmwareSearchPaths())
        if (QFileInfo(p).isFile())
            return p;
    return {};
}

QString firmwareMissingTitle()
{
    return QCoreApplication::translate("jScan", "Epson firmware missing");
}

QString firmwareMissingText()
{
    QStringList paths = firmwareSearchPaths();
    if (qEnvironmentVariableIsEmpty("JSCAN_V600_FIRMWARE"))
        paths.prepend(QStringLiteral("$JSCAN_V600_FIRMWARE"));
    const QString perUser = perUserFirmwarePath();
    return QCoreApplication::translate(
               "jScan",
               "The Epson Perfection V600 Photo needs Epson's firmware file, which is not "
               "included with jScan for legal reasons. It is loaded into the scanner every "
               "time the scanner is switched on.\n\n"
               "Missing file:\n%1 (65 793 bytes)\n\n"
               "Please copy it to:\n%2\n\n"
               "Searched locations:\n%3\n\n"
               "Where to get it: Fedora package “iscan-firmware”, Arch Linux AUR package "
               "“iscan-plugin-gt-x820”, or Epson's Linux driver download for the Perfection "
               "V600 Photo (bundle “iscan-gt-x820-bundle”; the file is usr/share/iscan/%1 in "
               "the package iscan-plugin-gt-x820). Details are in the README.")
        .arg(QString::fromLatin1(kFwName), perUser, paths.join(QLatin1Char('\n')));
}

QString firmwareMissingError()
{
    return QCoreApplication::translate("jScan", "The Epson firmware file %1 is missing.")
        .arg(QString::fromLatin1(kFwName));
}

QSizeF glassMm()
{
    return QSizeF(kGlassW, kGlassH);
}

QList<int> resolutions()
{
    return {100, 150, 200, 300, 400, 600, 800, 1200, 1600, 2400, 3200, 4800, 6400};
}

bool supports16Bit()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 13, 0)
    return true;
#else
    return false;
#endif
}

QImage scan(const QRectF &areaIn, bool gray, int dpi, int depth, const std::atomic_bool &cancel,
            const Callbacks &cb, QString *error)
{
    const bool deep = depth == 16 && supports16Bit();
    const unsigned bps = deep ? 2 : 1;   // bytes per sample of the final pass
    auto status = [&](const QString &t) {
        if (cb.status)
            cb.status(t);
    };
    const QString cancelled = QCoreApplication::translate("jScan", "Cancelled.");

    const Mode &mode = modeFor(dpi);
    const unsigned res = unsigned(mode.dpi);
    // pixels per x unit: 2 at 3200 dpi (one from each staggered CCD row)
    const unsigned ppu = res / mode.xUnitDpi;

    // ---- scan window at the optical resolution -----------------------------
    QRectF area = areaIn.normalized();
    area.setLeft(std::max(0.0, area.left()));
    area.setTop(std::max(0.0, area.top()));
    area.setRight(std::min(kGlassW, area.right()));
    area.setBottom(std::min(kGlassH, area.bottom()));
    if (area.width() < 1 || area.height() < 1) {
        *error = QCoreApplication::translate("jScan", "The scan area is empty.");
        return {};
    }
    const double dpm = res / 25.4;
    // Window in sensor x units (= pixels up to 1600 dpi): glass edge at
    // mode.glassStart; end - start is the number of pixels.
    const double upm = mode.xUnitDpi / 25.4;
    const unsigned glassUnits = unsigned(kGlassW * upm) / 16 * 16;
    const unsigned glassEnd = mode.glassStart + glassUnits;
    const unsigned xUnits = unsigned(std::lround(area.left() * upm));
    const unsigned xPx = unsigned(std::lround(area.left() * dpm));
    const unsigned yPx = unsigned(std::lround(area.top() * dpm));
    unsigned outW = std::max(8u, unsigned(area.width() * dpm) / 8 * 8);
    const unsigned outH = std::max(1u, unsigned(area.height() * dpm));
    unsigned devW = (outW + 15) / 16 * 16;
    unsigned start = mode.glassStart + xUnits;
    if (start + devW / ppu > glassEnd) {
        devW = std::min(devW, glassUnits * ppu);
        start = glassEnd - devW / ppu;
    }
    while (start % mode.startMod != mode.startRes && start > 0) {
        --start;
        devW += ppu;
    }
    devW = (devW + 15) / 16 * 16;
    const unsigned end = start + devW;
    const unsigned cropX = (mode.glassStart + xUnits - start) * ppu;
    outW = std::min(outW, devW - cropX);
    if (double(outW) * outH * bps > 1.5e8) {
        *error = QCoreApplication::translate(
                     "jScan", "The scan area is too large for %1 dpi. Please choose a smaller area "
                              "or a lower resolution.")
                     .arg(res);
        return {};
    }
    // Carriage pre-feed to the first line, in 1600 dpi steps, with the long
    // acceleration ramp, or the short one close to the top edge. At 400 dpi
    // Epson's driver starts from the home position there instead and the
    // leading lines are dropped.
    const unsigned stepsPerLine = 1600 / std::min(res, 1600u);   // 4, 2, 1 (3200: 1 per 2 lines)
    int prefeed = int(yPx * std::min(res, 1600u) / res * stepsPerLine) + mode.prefeedOffset;
    bool shortRamp = false, fromHome = false;
    if (prefeed < 512) {
        if (mode.dpi == 400 && prefeed < 16)
            fromHome = true;
        else if (mode.dpi != 400) {
            shortRamp = true;
            prefeed += 256;
        }
    }
    const unsigned skip = fromHome ? yPx : 0;
    const unsigned colourOffset = mode.colourOffset;
    // Resampling of unevenly placed CCD phases: output pixel of phase k lies
    // between samples x + phaseFrom[k] and x + phaseFrom[k] + 1 (weight phaseT).
    int phaseFrom[4] = {0, 0, 0, 0};
    double phaseT[4] = {0, 0, 0, 0};
    for (unsigned k = 0; k < mode.phases; ++k) {
        const int n = int(mode.phases);
        auto pos = [&](int j) {   // position of sample j relative to the group of phase 0
            const int q = j >= 0 ? j / n : -((-j + n - 1) / n);
            return q * n + mode.phasePos[j - q * n];
        };
        const double target = k;
        for (int j = int(k) - n; j < int(k) + n; ++j)
            if (pos(j) <= target && target < pos(j + 1)) {
                phaseFrom[k] = j - int(k);
                phaseT[k] = (target - pos(j)) / (pos(j + 1) - pos(j));
                break;
            }
    }
    const uint32_t lines = outH + colourOffset + mode.extraLines + skip;
    // Line clock: fixed up to 1600 dpi; above it grows with the width
    // (period about 0.53 per pixel, Epson at 3200 dpi: 1264 px -> 7760,
    // 2512 px -> 8072).
    unsigned scanClock = mode.lineClock;
    if (mode.scanClock1264) {
        const unsigned period = 2 * mode.scanClock1264 - 8
                                + unsigned(std::ceil(std::max(0, int(devW) - 1264) * 0.527));
        scanClock = ((period + 7) / 8 * 8 + 8) / 2;
    }
    const unsigned scanPeriod = 2 * scanClock - 8;

    // ---- connect, firmware -------------------------------------------------
    status(QCoreApplication::translate("jScan", "Connecting to the scanner …"));
    Usb usb;
    if (const int r = usb.open()) {
        *error = openError(r);
        return {};
    }
    auto ioError = [&]() {
        *error = QCoreApplication::translate("jScan", "Communication with the scanner failed (%1).").arg(usb.lastError());
        return QImage();
    };

    unsigned char st[2];
    if (!usb.status(st))
        return ioError();
    if (!(st[0] & 0x10)) {
        Bytes fw;
        if (!loadFirmware(&fw, error))
            return {};
        status(QCoreApplication::translate("jScan", "Loading the firmware into the scanner …"));
        if (!uploadFirmware(usb, fw))
            return ioError();
    }
    status(QCoreApplication::translate("jScan", "Waiting for the scanner …"));
    if (!usb.waitIdle(90))
        return ioError();
    if (cancel)
        return *error = cancelled, QImage();

    // ---- session setup ------------------------------------------------------
    status(QCoreApplication::translate("jScan", "Calibrating …"));
    if (!usb.write({0x1e, 0xe1}) || !usb.ack() || !usb.write({0x0b, 0x00, 0x54, 0x63, 0x00, 0x00})
        || !usb.ack() || !usb.write({0x1b, 0x40}) || !usb.ack() || !usb.waitIdle())
        return ioError();

    Bytes identity(256);
    for (unsigned i = 0; i < 256; ++i)
        identity[i] = static_cast<unsigned char>(i);
    for (unsigned char t : {0xfc, 0xfd, 0xfe})
        if (!usb.table({0x03, 0x00, t, 0x1f, 0x02, 0x00, 0x01, 0x00}, identity.data(), 256))
            return ioError();

    Bytes p57(42, 0);
    put32(p57, 0, res);
    put32(p57, 4, res);
    put32(p57, 8, xPx);
    put32(p57, 12, yPx);
    put32(p57, 16, outW);
    put32(p57, 20, outH);
    put32(p57, 24, gray ? 0x0800 : 0x0813);
    put32(p57, 28, 0x04fe);
    p57[33] = 0x80;
    if (!usb.cmd(0x57, p57) || !usb.cmd(0xa2, {0x0a}) /* lamp on */ || !usb.waitIdle())
        return ioError();

    Bytes r87(10), r86(4), cal(16);
    if (!usb.cmd(0x87) || !usb.write({0x00}) || !usb.read(r87.data(), 10)
        || !usb.write({0x1e, 0x86}) || !usb.read(r86.data(), 4) || !usb.cmd(0x93)
        || !usb.write({0x04}) || !usb.read(cal.data(), 16))
        return ioError();
    // AFE gains, offsets and exposure: stored in the scanner for 400 dpi
    Bytes afe31(12, 0);
    unsigned exposure = mode.exposure;
    if (mode.storedCalibration) {
        std::copy(cal.begin(), cal.begin() + 11, afe31.begin());
        exposure = cal[11] | (cal[12] << 8);
    } else {
        for (size_t c = 0; c < 3; ++c) {
            afe31[2 * c] = mode.gain;
            afe31[8 + c] = mode.offset[c];
        }
    }

    Bytes exp22, expDark22;
    auto setExposure = [&](unsigned e) {
        exp22.assign(12, 0);
        put16(exp22, 0, e);
        put16(exp22, 3, e);
        exp22[10] = mode.flag21;
        expDark22.assign(12, 0);
        put16(expDark22, 0, e);
        expDark22[9] = 0x01;
        expDark22[10] = mode.flag21;
    };
    setExposure(exposure);
    // line clock + CCD timing; up to 1600 dpi one fixed setting for everything
    auto setClock = [&](unsigned clock, const Bytes &table) {
        Bytes p21(26, 0);
        put16(p21, 0, clock);
        return usb.cmd(0x21, p21)
               && usb.table({0x07, 0, 0, 0, 0, 0, 0x01, 0}, table.data(), table.size());
    };
    const bool dynamicTiming = mode.dpi > 1600;
    const Bytes table07(kTable07, kTable07 + sizeof kTable07);
    const Bytes cal43 = {0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x09, 0x78, 0xec,
                         0x79, 0xf2, 0x7a, 0, 0, 0, 0, 0, 0};
    const Bytes fin43 = {0x09, 0x78, 0xec, 0x79, 0xf2, 0x7a, 0x09, 0x78, 0xec,
                         0x79, 0xf2, 0x7a, 0, 0, 0, 0, 0, 0};
    const Bytes zero42(24, 0);
    const Bytes zero4(4, 0);

    if (!usb.cmd(0x5a, zero4) || !usb.cmd(0x11, {mode.sensorMode}) || !usb.cmd(0x31, afe31)
        || !setClock(mode.lineClock,
                     dynamicTiming ? timingTable(mode.passAPeriod ? mode.passAPeriod
                                                                  : 2 * mode.lineClock - 8,
                                                 mode.fullStart, mode.fullEnd, 0)
                                   : table07))
        return ioError();
    const unsigned char rows = mode.rows, f21 = mode.flag21;

    Bytes data;
    // pass A: full sensor width, 1 line, 8 bit (data not used)
    const unsigned fullStart = mode.fullStart, fullEnd = mode.fullEnd;
    if (!usb.cmd(0x22, exp22)
        || !usb.cmd(0x41, window41(fullStart, fullEnd, 1,
                                   {1, rows, mode.divider, 0, 0, 0, 0, 0, 0x80, 0, 1, 0, f21, 0}))
        || !usb.cmd(0x42, zero42) || !usb.cmd(0x43, cal43) || !usb.waitIdle()
        || !usb.startAndRead(data, size_t(fullEnd - fullStart) * 3 + 8))
        return ioError();

    // pass B: masked sensor pixels, 4 lines, 16 bit -> dark level estimate
    const unsigned darkW = mode.darkEnd - mode.darkStart;
    if (dynamicTiming && !setClock(scanClock, timingTable(scanPeriod, 0, 0, mode.darkEdge)))
        return ioError();
    if (!usb.waitIdle() || !usb.cmd(0x5a, zero4) || !usb.cmd(0x22, exp22)
        || !usb.cmd(0x41, window41(mode.darkStart, mode.darkEnd, 4,
                                   {0, rows, mode.divider, 0, 0, 0, 0, 0, 0x80, 0, 4, 0, f21, 0}))
        || !usb.cmd(0x42, zero42) || !usb.cmd(0x43, cal43) || !usb.waitIdle()
        || !usb.startAndRead(data, size_t(darkW) * 4 * 6 + 8))
        return ioError();
    double darkPre[3];
    channelMeans(data.data(), size_t(darkW) * 4 * 3, darkPre);
    if (cancel)
        return *error = cancelled, QImage();

    // pass C: white reference, 32 lines, 16 bit. If the white level is
    // clipped or far too dark, adjust the exposure and measure again.
    Bytes p42(24, 0);
    for (size_t c = 0; c < 3; ++c) {
        put16(p42, 6 + 4 * c, unsigned(std::lround(darkPre[c])));
        put16(p42, 8 + 4 * c, unsigned(std::lround(darkPre[c])));
    }
    // motor speeds follow the line clock
    Bytes whiteMotor(2);
    put16(whiteMotor, 0, scanClock / 8);
    if (dynamicTiming && !setClock(scanClock, timingTable(scanPeriod, start, end, 0)))
        return ioError();
    Bytes white;
    for (int attempt = 0;; ++attempt) {
        if (!usb.waitIdle() || !usb.cmd(0x5a, zero4) || !usb.cmd(0x22, exp22)
            || !usb.cmd(0x41, window41(start, end, 32,
                                       {0, 2, 1, 0x40, 0, 0, 0, 0, 0x80, 0x80, 32, 0, f21, 0}))
            || !usb.cmd(0x42, p42) || !usb.cmd(0x43, cal43)
            || !usb.table({0x05, 0, 0, 0x01, 0x02, 0x02, 0, 0}, whiteMotor.data(), 2)
            || !usb.cmd(0x01, {0x14, 0, 0, 0, 0x04, 0, 0, 0, 0, 0, 0x08, 0}) || !usb.waitIdle()
            || !usb.startAndRead(white, size_t(devW) * 3 * 2 * 32 + 8))
            return ioError();
        if (mode.storedCalibration || attempt >= 3)
            break;
        // brightest 1 % of the samples
        std::vector<unsigned> v(size_t(devW) * 3 * 32);
        for (size_t i = 0; i < v.size(); ++i)
            v[i] = white[2 * i] | (white[2 * i + 1] << 8);
        std::nth_element(v.begin(), v.begin() + long(v.size() * 99 / 100), v.end());
        const double top = v[v.size() * 99 / 100];
        if (top < 64000 && top > 30000)
            break;
        exposure = unsigned(std::clamp(exposure * 52000.0 / std::max(top, 1000.0), 500.0, 30000.0));
        setExposure(exposure);
    }

    // pass D: dark reference with the lamp off, 16 lines
    Bytes dark;
    const Bytes winD = window41(start, end, 16, {0, rows, mode.divider, 0, 0, 0, 0, 0, 0x80, 0, 16, 0, f21, 0});
    if (!usb.waitIdle() || !usb.cmd(0xa2, {0x0b}) || !usb.cmd(0x5a, zero4)
        || !usb.cmd(0x22, expDark22) || !usb.cmd(0x41, winD) || !usb.cmd(0x42, zero42)
        || !usb.cmd(0x43, cal43) || !usb.cmd(0x41, winD) || !usb.waitIdle()
        || !usb.startAndRead(dark, size_t(devW) * 3 * 2 * 16 + 8))
        return ioError();
    double darkLvl[3];
    channelMeans(dark.data(), size_t(devW) * 3 * 16, darkLvl);
    if (cancel)
        return *error = cancelled, QImage();

    // Shading table: per pixel white level above the per-channel minimum,
    // scaled by the largest power of two that still fits 16 bit.
    std::vector<double> wm(size_t(devW) * 3, 0.0);
    for (size_t l = 0; l < 32; ++l)
        for (size_t i = 0; i < wm.size(); ++i) {
            const size_t o = (l * wm.size() + i) * 2;
            wm[i] += white[o] | (white[o + 1] << 8);
        }
    double wmin[3] = {1e9, 1e9, 1e9}, wmax[3] = {0, 0, 0};
    for (size_t i = 0; i < wm.size(); ++i) {
        wm[i] /= 32.0;
        wmin[i % 3] = std::min(wmin[i % 3], wm[i]);
        wmax[i % 3] = std::max(wmax[i % 3], wm[i]);
    }
    double range = 0;
    for (int c = 0; c < 3; ++c)
        range = std::max(range, wmax[c] - std::floor(wmin[c]));
    int k = 0;
    while (k < 3 && range * double(1 << (k + 1)) <= 65535.0)
        ++k;
    Bytes shading(wm.size() * 2);
    for (size_t i = 0; i < wm.size(); ++i) {
        const double v = (wm[i] - std::floor(wmin[i % 3])) * double(1 << k);
        put16(shading, 2 * i, unsigned(std::clamp(std::lround(v), 0L, 65535L)));
    }

    // lamp on again, carriage to the first line
    Bytes pre01(12, 0);
    put32(pre01, 0, uint32_t(std::max(prefeed, 0)));
    pre01[6] = 0xff;
    pre01[7] = shortRamp ? 0x00 : 0x01;
    pre01[8] = 0xff;
    pre01[10] = 0x10;
    if (!usb.waitIdle() || !usb.cmd(0xa2, {0x0a}) || !usb.write({0x1e, 0x86})
        || !usb.read(r86.data(), 4) || !usb.cmd(0x5a, {2, 0, 0, 0}))
        return ioError();
    if (!fromHome
        && (!(shortRamp ? usb.table({0x05, 0, 0, 0x01, 0x02, 0, 0x02, 0}, kMotorPrefeedShort,
                                    sizeof kMotorPrefeedShort)
                        : usb.table({0x05, 0, 0, 0x01, 0x02, 0, 0x06, 0}, kMotorPrefeed400,
                                    sizeof kMotorPrefeed400))
            || !usb.cmd(0x01, pre01) || !usb.cmd(0x05) || !usb.waitIdle()))
        return ioError();

    // ---- final pass -----------------------------------------------------------
    const unsigned lpb = std::max(1u, 51192u / (devW * 3 * bps));
    const size_t block = size_t(lpb) * devW * 3 * bps + 8;
    Bytes fin42(24, 0);
    for (size_t c = 0; c < 3; ++c) {
        put16(fin42, 2 * c, unsigned(std::floor(wmin[c])));
        put16(fin42, 6 + 4 * c, unsigned(std::lround(darkLvl[c])));
        put16(fin42, 8 + 4 * c, unsigned(std::lround(darkLvl[c])));
    }
    Bytes shHdr = {0x06, 0x00, 0x10, 0x01, 0x02, 0, 0, 0};
    put16(shHdr, 5, unsigned(shading.size()));
    Bytes fin01(12, 0);
    Bytes scanMotor(2);
    put16(scanMotor, 0, scanClock * res / 25600);
    if (!mode.rampScan) {   // constant speed
        put32(fin01, 0, lines * stepsPerLine * std::min(res, 1600u) / res + mode.runout);
        fin01[4] = mode.runout;
        fin01[10] = 0x08;
    } else {                // acceleration ramp (400 dpi)
        if (fromHome) {
            put32(fin01, 0, lines * 4 + 128 + 392);
            fin01[4] = 0x08;
            fin01[5] = 0x02;
        } else {
            put32(fin01, 0, lines * 4 + 128);
            fin01[4] = 0x80;
        }
        fin01[6] = 0xff;
        fin01[7] = 0x01;
        fin01[8] = 0xff;
    }
    const unsigned char b15 = static_cast<unsigned char>(((5 - k) << 4) | mode.final15);
    if (!usb.cmd(0x22, exp22)
        || !usb.cmd(0x41, window41(start, end, lines,
                                   {static_cast<unsigned char>(deep ? 0 : 1), rows, mode.divider, mode.final11, 0x80, 0, 0, b15, 0x80, 0xa0,
                                    static_cast<unsigned char>(lpb), 0, f21, 0}))
        || !usb.cmd(0x42, fin42) || !usb.cmd(0x43, fin43)
        || !usb.table(shHdr, shading.data(), shading.size())
        || !(!mode.rampScan
                 ? usb.table({0x05, 0, 0, 0x01, 0x02, 0x02, 0, 0}, scanMotor.data(), 2)
                 : usb.table({0x05, 0, 0, 0x01, 0x02, 0, 0x06, 0}, kMotorScan400, sizeof kMotorScan400))
        || !usb.cmd(0x01, fin01) || !usb.waitIdle() || !usb.write({0x1e, 0x47}))
        return ioError();

    const size_t lineBytes = size_t(devW) * 3 * bps;
    const size_t total = lineBytes * lines;
    BlockQueue q;
    std::thread reader([&] {
        size_t received = 0;
        while (received < total) {
            Bytes b(std::min(block, total - received + 8));
            const int got = usb.readChunk(b.data(), b.size(), 30000);
            if (got <= 8) {
                std::lock_guard<std::mutex> lock(q.m);
                q.failed = true;
                break;
            }
            b.resize(size_t(got) - 8);   // strip the block trailer
            received += b.size();
            std::lock_guard<std::mutex> lock(q.m);
            q.blocks.push_back(std::move(b));
            q.cv.notify_one();
        }
        std::lock_guard<std::mutex> lock(q.m);
        q.done = true;
        q.cv.notify_one();
    });

    status(QCoreApplication::translate("jScan", "Scanning document …"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 13, 0)
    const QImage::Format fmt = deep ? (gray ? QImage::Format_Grayscale16 : QImage::Format_RGBX64)
                                    : (gray ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
#else
    const QImage::Format fmt = gray ? QImage::Format_Grayscale8 : QImage::Format_RGB888;
#endif
    QImage img(int(outW), int(outH), fmt);
    // Raw lines still needed for the colour alignment; lines below
    // rawFirst have been processed and released.
    Bytes raw;
    std::vector<double> rowIn(size_t(outW) * 3);
    size_t rawFirst = 0;
    unsigned rowsDone = 0;
    QElapsedTimer liveTimer;
    liveTimer.start();
    // samples are handled as 0..1; written as 8 or 16 bit
    auto u8 = [](double v) { return static_cast<uchar>(std::clamp(std::lround(v * 255.0), 0L, 255L)); };
    auto u16 = [](double v) {
        return static_cast<quint16>(std::clamp(std::lround(v * 65535.0), 0L, 65535L));
    };
    auto sample = [deep](const unsigned char *p) {
        return deep ? (p[0] | (p[1] << 8)) / 65535.0 : p[0] / 255.0;
    };
    for (;;) {
        std::deque<Bytes> got;
        bool done = false, failed = false;
        {
            std::unique_lock<std::mutex> lock(q.m);
            q.cv.wait_for(lock, std::chrono::milliseconds(150),
                          [&] { return !q.blocks.empty() || q.done; });
            got.swap(q.blocks);
            done = q.done;
            failed = q.failed;
        }
        if (cancel) {   // let the reader drain the scanner, discard the data
            reader.join();
            return *error = cancelled, QImage();
        }
        for (const Bytes &b : got)
            raw.insert(raw.end(), b.begin(), b.end());

        // align the colour lines and apply the colour matrix
        const size_t avail = rawFirst + raw.size() / lineBytes;
        while (rowsDone < outH && skip + rowsDone + colourOffset + mode.extraLines < avail) {
            const size_t y = skip + rowsDone - rawFirst;
            const size_t px = 3 * bps;   // bytes per raw pixel
            const unsigned char *r = raw.data() + (y + colourOffset) * lineBytes + px * cropX;
            const unsigned char *g = raw.data() + (y + colourOffset / 2) * lineBytes + px * cropX + bps;
            const unsigned char *b = raw.data() + y * lineBytes + px * cropX + 2 * bps;
            // staggered CCD rows: pixel x arrives phaseLines[x % phases] lines later
            for (unsigned x = 0; x < outW; ++x) {
                const size_t o = size_t(mode.phaseLines[(cropX + x) % mode.phases]) * lineBytes + px * x;
                rowIn[3 * x] = sample(r + o);
                rowIn[3 * x + 1] = sample(g + o);
                rowIn[3 * x + 2] = sample(b + o);
            }
            uchar *dst = img.scanLine(int(rowsDone));
            for (unsigned x = 0; x < outW; ++x) {
                double in[3];
                const unsigned k = (cropX + x) % mode.phases;
                if (phaseT[k] == 0 && phaseFrom[k] == 0) {
                    for (int i = 0; i < 3; ++i)
                        in[i] = rowIn[3 * x + unsigned(i)];
                } else {
                    const int last = int(outW) - 1;
                    const unsigned a = unsigned(std::clamp(int(x) + phaseFrom[k], 0, last));
                    const unsigned bb = unsigned(std::clamp(int(x) + phaseFrom[k] + 1, 0, last));
                    for (int i = 0; i < 3; ++i)
                        in[i] = rowIn[3 * a + unsigned(i)] * (1 - phaseT[k])
                                + rowIn[3 * bb + unsigned(i)] * phaseT[k];
                }
                double c[3];
                for (int i = 0; i < 3; ++i)
                    c[i] = kCct[i][0] * in[0] + kCct[i][1] * in[1] + kCct[i][2] * in[2];
                if (deep) {
                    auto *d16 = reinterpret_cast<quint16 *>(dst);
                    if (gray) {
                        d16[x] = u16(0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]);
                    } else {
                        d16[4 * x] = u16(c[0]);
                        d16[4 * x + 1] = u16(c[1]);
                        d16[4 * x + 2] = u16(c[2]);
                        d16[4 * x + 3] = 0xffff;
                    }
                } else if (gray) {
                    dst[x] = u8(0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]);
                } else {
                    dst[3 * x] = u8(c[0]);
                    dst[3 * x + 1] = u8(c[1]);
                    dst[3 * x + 2] = u8(c[2]);
                }
            }
            ++rowsDone;
        }
        // release processed lines (keeps memory low at high resolutions)
        const size_t used = std::min<size_t>(skip + rowsDone, rawFirst + raw.size() / lineBytes) - rawFirst;
        if (used * lineBytes > (16u << 20)) {
            raw.erase(raw.begin(), raw.begin() + long(used * lineBytes));
            rawFirst += used;
        }

        if (failed) {
            reader.join();
            return ioError();
        }
        if (done && rowsDone >= outH)
            break;
        if (done) {   // reader finished but lines are missing
            reader.join();
            *error = QCoreApplication::translate("jScan", "The scanner delivered too little image data.");
            return {};
        }
        if (cb.progress && liveTimer.elapsed() > 150 && rowsDone > 0) {
            liveTimer.restart();
            const int w = std::min<int>(int(outW), 900);
            const double f = double(w) / outW;
            cb.progress(img.copy(0, 0, int(outW), int(rowsDone)).scaledToWidth(w, Qt::FastTransformation),
                        int(outH * f));
            if (cb.status)
                cb.status(QCoreApplication::translate("jScan", "Scanning document … %1 %").arg(int(rowsDone * 100 / outH)));
        }
    }
    reader.join();

    if (dpi > 0 && dpi < mode.dpi) {
        const int w = std::max(1, int(std::lround(outW * double(dpi) / mode.dpi)));
        const int h = std::max(1, int(std::lround(outH * double(dpi) / mode.dpi)));
        img = img.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    } else {
        dpi = mode.dpi;
    }
    const int dpmOut = int(std::lround(dpi / 0.0254));
    img.setDotsPerMeterX(dpmOut);
    img.setDotsPerMeterY(dpmOut);
    return img;
}

} // namespace V600
