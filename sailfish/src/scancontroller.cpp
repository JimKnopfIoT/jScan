#include "scancontroller.h"

#include <QCoreApplication>
#include "pdfexport.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTransform>
#include <QtConcurrent>
#include <algorithm>

namespace {
QString sourceLabel(const QString &s)
{
    if (s == QLatin1String("ADF Duplex")) return QCoreApplication::translate("jScan", "Feeder – duplex");
    if (s == QLatin1String("ADF Front")) return QCoreApplication::translate("jScan", "Feeder – front");
    if (s == QLatin1String("ADF Back")) return QCoreApplication::translate("jScan", "Feeder – back");
    if (s == QLatin1String("ADF")) return QCoreApplication::translate("jScan", "Feeder");
    if (s == QLatin1String("Flatbed")) return QCoreApplication::translate("jScan", "Flatbed");
    if (s == QLatin1String("Card Front")) return QCoreApplication::translate("jScan", "Card slot – front");
    if (s == QLatin1String("Card Duplex")) return QCoreApplication::translate("jScan", "Card slot – duplex");
    return s;
}

QString modeName(ImageOps::ColorMode m)
{
    switch (m) {
    case ImageOps::ColorMode::Gray:    return QStringLiteral("Gray");
    case ImageOps::ColorMode::Lineart: return QStringLiteral("Lineart");
    default:                           return QStringLiteral("Color");
    }
}

} // namespace

ScanController::ScanController(QObject *parent) : QObject(parent)
{
    m_reprocess.setSingleShot(true);
    m_reprocess.setInterval(250);
    connect(&m_reprocess, &QTimer::timeout, this, &ScanController::applyPageSettings);

    m_worker = new ScanWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &ScanWorker::status, this, &ScanController::setStatus);
    connect(m_worker, &ScanWorker::devicesFound, this, &ScanController::onDevices);
    connect(m_worker, &ScanWorker::capsReady, this, &ScanController::onCaps);
    connect(m_worker, &ScanWorker::pageScanned, this, &ScanController::onPage);
    connect(m_worker, &ScanWorker::pageProgress, this, &ScanController::onProgress);
    connect(m_worker, &ScanWorker::finished, this, &ScanController::onFinished);
    connect(m_worker, &ScanWorker::notice, this, &ScanController::notice);
    m_thread.start();

    loadSettings();
    refreshDevices();
}

ScanController::~ScanController()
{
    saveSettings();
    QThreadPool::globalInstance()->waitForDone();
    m_worker->requestCancel();
    m_thread.quit();
    m_thread.wait(3000);
}

// --- devices ------------------------------------------------------------------

QStringList ScanController::devices() const
{
    QStringList l;
    for (const ScanDevice &d : m_devices)
        l << d.label;
    return l;
}

void ScanController::refreshDevices()
{
    if (m_state != Idle)
        return;
    setState(Searching);
    QMetaObject::invokeMethod(m_worker, "listDevices", Qt::QueuedConnection);
}

void ScanController::onDevices(const QList<ScanDevice> &found)
{
    if (m_deviceIndex >= 0 && m_deviceIndex < m_devices.size())
        m_prefDevice = m_devices[m_deviceIndex].key;
    m_devices = found;
    // network scanner entered in the settings (no mDNS discovery on the phone)
    if (!m_networkHost.trimmed().isEmpty()) {
        QString h = m_networkHost.trimmed();
        ScanDevice d;
        d.name = h.startsWith(QLatin1String("http")) ? QStringLiteral("escl:") + h
                                                     : QStringLiteral("escl:https://%1:443").arg(h);
        d.key = QStringLiteral("eSCL ") + h;
        d.label = QCoreApplication::translate("jScan", "Network scanner (%1)").arg(h);
        m_devices << d;
    }
    m_deviceIndex = -1;
    for (int i = 0; i < m_devices.size(); ++i)
        if (m_devices[i].key == m_prefDevice)
            m_deviceIndex = i;
    if (m_deviceIndex < 0 && !m_devices.isEmpty())
        m_deviceIndex = 0;
    emit devicesChanged();
    emit deviceIndexChanged();
    setState(Idle);
    if (m_devices.isEmpty()) {
        m_caps = DeviceCaps();
        emit capsChanged();
        setStatus(QCoreApplication::translate("jScan", "No scanner found"));
        return;
    }
    queryCaps();
}

void ScanController::setDeviceIndex(int i)
{
    if (i == m_deviceIndex || i < 0 || i >= m_devices.size() || m_state != Idle)
        return;
    saveSettings();
    m_deviceIndex = i;
    m_prefDevice = m_devices[i].key;
    emit deviceIndexChanged();
    queryCaps();
}

void ScanController::queryCaps()
{
    m_caps = DeviceCaps();
    emit capsChanged();
    if (m_deviceIndex < 0)
        return;
    setState(Querying);
    QMetaObject::invokeMethod(m_worker, "queryCaps", Qt::QueuedConnection,
                              Q_ARG(QString, m_devices[m_deviceIndex].name));
}

void ScanController::onCaps(const DeviceCaps &caps, const QString &err)
{
    setState(Idle);
    if (m_deviceIndex < 0 || caps.device != m_devices[m_deviceIndex].name)
        return;
    if (!err.isEmpty()) {
        setStatus(err);
        return;
    }
    m_caps = caps;

    QSettings st;
    st.beginGroup(QStringLiteral("devices/") + m_devices[m_deviceIndex].key);
    const QString src = st.value(QStringLiteral("source"), QStringLiteral("ADF Duplex")).toString();
    const int dpi = st.value(QStringLiteral("dpi"), 300).toInt();
    st.endGroup();

    m_sourceIndex = std::max(0, int(m_caps.sources.indexOf(src)));
    m_dpiIndex = 0;
    for (int i = 0; i < m_caps.resolutions.size(); ++i)
        if (std::abs(m_caps.resolutions[i] - dpi) < std::abs(m_caps.resolutions[m_dpiIndex] - dpi))
            m_dpiIndex = i;
    emit capsChanged();
    emit settingsChanged();
    emit stateChanged();
    setStatus(QCoreApplication::translate("jScan", "Ready: %1").arg(m_devices[m_deviceIndex].label));
}

QStringList ScanController::sources() const
{
    QStringList l;
    for (const QString &s : m_caps.sources)
        l << sourceLabel(s);
    return l;
}

void ScanController::setSourceIndex(int i)
{
    if (i == m_sourceIndex || i < 0 || i >= m_caps.sources.size())
        return;
    m_sourceIndex = i;
    emit settingsChanged();
    saveSettings();
}

QVariantList ScanController::resolutions() const
{
    QVariantList l;
    for (int r : m_caps.resolutions)
        l << r;
    return l;
}

void ScanController::setDpiIndex(int i)
{
    if (i == m_dpiIndex || i < 0 || i >= m_caps.resolutions.size())
        return;
    m_dpiIndex = i;
    emit settingsChanged();
    saveSettings();
}

bool ScanController::feeder() const
{
    return m_sourceIndex < m_caps.sources.size()
           && ScanWorker::isFeeder(m_caps.sources[m_sourceIndex]);
}

QString ScanController::feedHint() const
{
    return m_caps.device.startsWith(QLatin1String("canon_dr"))
               ? QCoreApplication::translate("jScan", "Put the sheets into the feeder face down and upside down, "
                                                   "then tap “Scan”.")
               : QCoreApplication::translate("jScan", "Put the sheets into the feeder and tap “Scan”.");
}

bool ScanController::ocrAvailable() const
{
    return !PdfExport::tesseractPath().isEmpty() && !PdfExport::ocrLanguages().isEmpty();
}

void ScanController::setOcr(bool b)
{
    if (b == m_ocr)
        return;
    m_ocr = b;
    emit settingsChanged();
    saveSettings();
}

// --- page settings: apply to the current page and to the next scans -------

void ScanController::setColorMode(int m)
{
    m = std::max(0, std::min(2, m));
    if (m == int(m_ps.mode))
        return;
    m_ps.mode = ImageOps::ColorMode(m);
    pageSettingChanged();
}

void ScanController::setContrast(int c)
{
    if (c == m_ps.contrast)
        return;
    m_ps.contrast = c;
    pageSettingChanged();
}

void ScanController::setFlatten(int f)
{
    if (f == m_ps.flatten)
        return;
    m_ps.flatten = f;
    pageSettingChanged();
}

void ScanController::setDeskew(bool d)
{
    if (d == m_ps.deskew)
        return;
    m_ps.deskew = d;
    pageSettingChanged();
}

void ScanController::pageSettingChanged()
{
    emit pageSettingsChanged();
    m_reprocess.start();   // sliders: wait until the finger rests
}

void ScanController::applyPageSettings()
{
    saveSettings();
    if (m_current < 0)
        return;
    {
        QMutexLocker lock(&m_mutex);
        if (m_current >= m_pages.size() || m_pages[m_current].ps == m_ps)
            return;
        m_pages[m_current].ps = m_ps;
    }
    processPage(m_current);
}

void ScanController::setSkipBlank(bool b)
{
    if (b == m_skipBlank)
        return;
    m_skipBlank = b;
    emit settingsChanged();
    saveSettings();
}

void ScanController::setCompress(bool b)
{
    if (b == m_compress)
        return;
    m_compress = b;
    emit settingsChanged();
    saveSettings();
}

void ScanController::setNetworkHost(const QString &h)
{
    if (h == m_networkHost)
        return;
    m_networkHost = h;
    emit settingsChanged();
    saveSettings();
}

// --- scanning -----------------------------------------------------------------

void ScanController::scan()
{
    if (!ready())
        return;
    ScanSettings s;
    s.device = m_caps.device;
    s.source = m_sourceIndex < m_caps.sources.size() ? m_caps.sources[m_sourceIndex] : QString();
    // scan in the chosen mode if the scanner has it (grey is much faster)
    const QString want = modeName(m_ps.mode);
    s.mode = m_caps.modes.contains(want) ? want
             : m_caps.modes.contains(QStringLiteral("Color")) ? QStringLiteral("Color")
                                                              : m_caps.modes.value(0);
    s.dpi = m_dpiIndex < m_caps.resolutions.size() ? m_caps.resolutions[m_dpiIndex] : 300;
    s.paperMm = QSizeF(210.0, 297.0);
    s.deskew = m_ps.deskew;
    m_skipped = 0;
    setState(Scanning);
    QMetaObject::invokeMethod(m_worker, "scan", Qt::QueuedConnection, Q_ARG(ScanSettings, s));
}

void ScanController::cancel()
{
    m_worker->requestCancel();
    setStatus(QCoreApplication::translate("jScan", "Cancelling …"));
}

void ScanController::onProgress(const QImage &partial, int expectedHeight)
{
    Q_UNUSED(expectedHeight);
    {
        QMutexLocker lock(&m_mutex);
        m_live = partial;
    }
    m_liveUrl = QStringLiteral("image://jscan/live/%1").arg(++m_liveSerial);
    emit liveImageChanged();
}

void ScanController::onPage(const QImage &img)
{
    if (m_skipBlank && feeder() && ImageOps::isBlank(img)) {
        ++m_skipped;
        return;
    }
    int index;
    {
        QMutexLocker lock(&m_mutex);
        Page p;
        p.id = m_nextId++;
        p.raw = img;
        p.ps = m_ps;
        m_pages << p;
        index = m_pages.size() - 1;
    }
    m_current = index;
    emit pagesChanged();
    emit currentPageChanged();
    emit currentImageChanged();
    processPage(index);
}

void ScanController::onFinished(int pages, const QString &err)
{
    setState(Idle);
    {
        QMutexLocker lock(&m_mutex);
        m_live = QImage();
    }
    m_liveUrl.clear();
    emit liveImageChanged();
    if (!err.isEmpty() && pages == 0) {
        setStatus(err);
        if (err != QCoreApplication::translate("jScan", "Cancelled."))
            emit error(err);
        return;
    }
    QString msg = pages == 1 ? QCoreApplication::translate("jScan", "One page scanned")
                             : QCoreApplication::translate("jScan", "%1 pages scanned").arg(pages);
    if (m_skipped > 0)
        msg += m_skipped == 1 ? QCoreApplication::translate("jScan", ", one blank page removed")
                              : QCoreApplication::translate("jScan", ", %1 blank pages removed").arg(m_skipped);
    setStatus(msg);
}

// --- page processing ---------------------------------------------------------------

void ScanController::processPage(int index)
{
    int id;
    quint64 gen;
    QImage raw;
    ImageOps::PageSettings ps;
    {
        QMutexLocker lock(&m_mutex);
        if (index < 0 || index >= m_pages.size())
            return;
        Page &p = m_pages[index];
        p.dirty = true;
        id = p.id;
        gen = ++p.gen;
        raw = p.raw;
        ps = p.ps;
    }
    QtConcurrent::run([this, id, gen, raw, ps] {
        const QImage out = ImageOps::process(raw, ps);
        QMetaObject::invokeMethod(this, "onProcessed", Qt::QueuedConnection, Q_ARG(int, id),
                                  Q_ARG(quint64, gen), Q_ARG(QImage, out));
    });
}

void ScanController::onProcessed(int id, quint64 gen, const QImage &img)
{
    int index;
    {
        QMutexLocker lock(&m_mutex);
        index = indexOfId(id);
        if (index < 0 || m_pages[index].gen != gen)
            return;
        m_pages[index].processed = img;
        m_pages[index].dirty = false;
    }
    emit pagesChanged();   // thumbnail URLs carry the generation
    if (index == m_current)
        emit currentImageChanged();
}

int ScanController::indexOfId(int id) const
{
    for (int i = 0; i < m_pages.size(); ++i)
        if (m_pages[i].id == id)
            return i;
    return -1;
}

int ScanController::pageCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_pages.size();
}

void ScanController::setCurrentPage(int i)
{
    ImageOps::PageSettings ps;
    {
        QMutexLocker lock(&m_mutex);
        if (i < 0 || i >= m_pages.size() || i == m_current)
            return;
        ps = m_pages[i].ps;
    }
    m_current = i;
    m_reprocess.stop();
    m_ps = ps;   // controls show the settings of the selected page
    emit pageSettingsChanged();
    emit currentPageChanged();
    emit currentImageChanged();
}

QStringList ScanController::thumbs() const
{
    QMutexLocker lock(&m_mutex);
    QStringList l;
    for (const Page &p : m_pages)
        l << QStringLiteral("image://jscan/page/%1/%2").arg(p.id).arg(p.processed.isNull() ? 0 : p.gen);
    return l;
}

QString ScanController::currentImage() const
{
    QMutexLocker lock(&m_mutex);
    if (m_current < 0 || m_current >= m_pages.size())
        return QString();
    const Page &p = m_pages[m_current];
    return QStringLiteral("image://jscan/page/%1/%2").arg(p.id).arg(p.processed.isNull() ? 0 : p.gen);
}

QImage ScanController::image(const QString &id, const QSize &requested)
{
    QImage img;
    {
        QMutexLocker lock(&m_mutex);
        const QStringList parts = id.split(QLatin1Char('/'));
        if (parts.value(0) == QLatin1String("live")) {
            img = m_live;
        } else if (parts.value(0) == QLatin1String("page")) {
            const int idx = indexOfId(parts.value(1).toInt());
            if (idx >= 0)
                img = m_pages[idx].processed.isNull() ? m_pages[idx].raw : m_pages[idx].processed;
        }
    }
    if (img.isNull())
        return img;
    if (requested.isValid() && (requested.width() > 0 || requested.height() > 0)) {
        const QSize target(requested.width() > 0 ? requested.width() : img.width(),
                           requested.height() > 0 ? requested.height() : img.height());
        if (target.width() < img.width() || target.height() < img.height())
            img = img.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (img.format() == QImage::Format_Mono)   // scene graph prefers 32 bit
        img = img.convertToFormat(QImage::Format_RGB32);
    return img;
}

void ScanController::rotatePage(int index, int degrees)
{
    {
        QMutexLocker lock(&m_mutex);
        if (index < 0 || index >= m_pages.size())
            return;
        QTransform t;
        t.rotate(degrees);
        Page &p = m_pages[index];
        p.raw = p.raw.transformed(t);
        if (!p.processed.isNull())
            p.processed = p.processed.transformed(t);
        ++p.gen;   // new URL so QML reloads
    }
    processPage(index);
    emit pagesChanged();
    if (index == m_current)
        emit currentImageChanged();
}

void ScanController::removePage(int index)
{
    {
        QMutexLocker lock(&m_mutex);
        if (index < 0 || index >= m_pages.size())
            return;
        m_pages.removeAt(index);
        if (m_current >= m_pages.size())
            m_current = m_pages.size() - 1;
    }
    emit pagesChanged();
    emit currentPageChanged();
    emit currentImageChanged();
}

void ScanController::movePage(int from, int to)
{
    {
        QMutexLocker lock(&m_mutex);
        if (from < 0 || to < 0 || from >= m_pages.size() || to >= m_pages.size() || from == to)
            return;
        m_pages.move(from, to);
        if (m_current == from)
            m_current = to;
    }
    emit pagesChanged();
    emit currentPageChanged();
}

void ScanController::clearPages()
{
    {
        QMutexLocker lock(&m_mutex);
        m_pages.clear();
        m_current = -1;
    }
    emit pagesChanged();
    emit currentPageChanged();
    emit currentImageChanged();
    setStatus(QCoreApplication::translate("jScan", "Ready"));
}

// --- saving ---------------------------------------------------------------------

namespace {

QString defaultSaveFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
           + QStringLiteral("/Scans");
}

// File name as typed by the user: no path separators, no extension.
QString cleanFileName(QString name, bool png)
{
    name = name.trimmed();
    name.replace(QLatin1Char('/'), QLatin1Char('_'));
    name.remove(QChar(0));
    const QString ext = png ? QStringLiteral(".png") : QStringLiteral(".pdf");
    if (name.endsWith(ext, Qt::CaseInsensitive))
        name.chop(ext.size());
    return name.trimmed();
}

} // namespace

QString ScanController::saveFolder() const
{
    return m_saveFolder.isEmpty() ? defaultSaveFolder() : m_saveFolder;
}

QString ScanController::defaultFileName() const
{
    return QStringLiteral("Scan_")
           + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
}

bool ScanController::fileExists(const QString &folder, const QString &name, bool png) const
{
    const QString n = cleanFileName(name, png);
    if (n.isEmpty())
        return false;
    const QString base = folder + QLatin1Char('/') + n;
    if (!png)
        return QFileInfo::exists(base + QStringLiteral(".pdf"));
    return pageCount() == 1 ? QFileInfo::exists(base + QStringLiteral(".png"))
                            : QFileInfo::exists(base + QStringLiteral("_01.png"));
}

void ScanController::save(bool png)
{
    saveAs(png, saveFolder(), defaultFileName());
}

void ScanController::saveAs(bool png, const QString &folderIn, const QString &nameIn)
{
    if (m_state != Idle || pageCount() == 0)
        return;
    const QString folder = folderIn.isEmpty() ? saveFolder() : QDir::cleanPath(folderIn);
    QString name = cleanFileName(nameIn, png);
    if (name.isEmpty())
        name = defaultFileName();
    if (folder != saveFolder()) {
        m_saveFolder = folder;
        saveSettings();
        emit saveFolderChanged();
    }
    QList<QImage> pages, raws;
    QList<ImageOps::PageSettings> pss;
    QList<bool> dirty;
    {
        QMutexLocker lock(&m_mutex);
        for (const Page &p : m_pages) {
            pages << p.processed;
            raws << p.raw;
            pss << p.ps;
            dirty << (p.dirty || p.processed.isNull());
        }
    }
    if (!QDir().mkpath(folder)) {
        emit error(QCoreApplication::translate("jScan", "Could not create the folder %1.").arg(folder));
        return;
    }
    const QString base = folder + QLatin1Char('/') + name;
    PdfOptions opt;
    opt.compress = m_compress;
    opt.ocr = m_ocr && !png;   // Tesseract is shipped in /usr/share/jscan
    setState(Saving);
    setStatus(QCoreApplication::translate("jScan", "Saving …"));

    QtConcurrent::run([this, pages, raws, pss, dirty, base, png, opt]() mutable {
        for (int i = 0; i < pages.size(); ++i)
            if (dirty[i])
                pages[i] = ImageOps::process(raws[i], pss[i]);
        QStringList files;
        QString err;
        if (png) {
            for (int i = 0; i < pages.size() && err.isEmpty(); ++i) {
                const QString f = pages.size() == 1
                                      ? base + QStringLiteral(".png")
                                      : QStringLiteral("%1_%2.png").arg(base).arg(i + 1, 2, 10, QLatin1Char('0'));
                if (pages[i].save(f, "PNG"))
                    files << f;
                else
                    err = QCoreApplication::translate("jScan", "Could not write %1.").arg(f);
            }
        } else {
            const QString f = base + QStringLiteral(".pdf");
            bool skipped = false;
            err = PdfExport::write(
                pages, f, opt,
                [this](const QString &m) {
                    QMetaObject::invokeMethod(this, "setStatus", Qt::QueuedConnection,
                                              Q_ARG(QString, m));
                },
                &skipped);
            if (err.isEmpty())
                files << f;
        }
        QMetaObject::invokeMethod(this, "onSaveDone", Qt::QueuedConnection,
                                  Q_ARG(QStringList, files), Q_ARG(QString, err));
    });
}

void ScanController::onSaveDone(const QStringList &files, const QString &err)
{
    setState(Idle);
    if (!err.isEmpty()) {
        setStatus(QCoreApplication::translate("jScan", "Error while saving"));
        emit error(err);
        return;
    }
    qint64 bytes = 0;
    for (const QString &f : files)
        bytes += QFileInfo(f).size();
    setStatus(QCoreApplication::translate("jScan", "Saved: %1 (%2 MB)")
                  .arg(files.size() == 1 ? QFileInfo(files.first()).fileName()
                                         : QCoreApplication::translate("jScan", "%1 PNG files").arg(files.size()))
                  .arg(bytes / 1048576.0, 0, 'f', 1));
    emit saved(files.value(0));
}

// --- misc -----------------------------------------------------------------------

void ScanController::setStatus(const QString &s)
{
    if (s == m_status)
        return;
    m_status = s;
    emit statusChanged();
}

void ScanController::setState(State s)
{
    if (s == m_state)
        return;
    m_state = s;
    emit stateChanged();
}

void ScanController::loadSettings()
{
    QSettings st;
    m_prefDevice = st.value(QStringLiteral("device")).toString();
    m_ps.contrast = st.value(QStringLiteral("contrast"), 0).toInt();
    m_ps.flatten = st.value(QStringLiteral("flatten"), 50).toInt();
    m_ps.deskew = st.value(QStringLiteral("deskew"), true).toBool();
    m_ps.mode = ImageOps::ColorMode(std::max(0, std::min(2, st.value(QStringLiteral("colorMode"), 0).toInt())));
    m_skipBlank = st.value(QStringLiteral("skipBlank"), true).toBool();
    m_compress = st.value(QStringLiteral("compress"), true).toBool();
    m_ocr = st.value(QStringLiteral("ocr"), true).toBool();
    m_networkHost = st.value(QStringLiteral("networkHost")).toString();
    m_saveFolder = st.value(QStringLiteral("saveFolder")).toString();
}

void ScanController::saveSettings() const
{
    QSettings st;
    if (m_deviceIndex >= 0 && m_deviceIndex < m_devices.size()) {
        st.setValue(QStringLiteral("device"), m_devices[m_deviceIndex].key);
        if (!m_caps.device.isEmpty()) {
            st.beginGroup(QStringLiteral("devices/") + m_devices[m_deviceIndex].key);
            st.setValue(QStringLiteral("source"), m_caps.sources.value(m_sourceIndex));
            st.setValue(QStringLiteral("dpi"), m_caps.resolutions.value(m_dpiIndex, 300));
            st.endGroup();
        }
    }
    st.setValue(QStringLiteral("contrast"), m_ps.contrast);
    st.setValue(QStringLiteral("flatten"), m_ps.flatten);
    st.setValue(QStringLiteral("deskew"), m_ps.deskew);
    st.setValue(QStringLiteral("colorMode"), int(m_ps.mode));
    st.setValue(QStringLiteral("skipBlank"), m_skipBlank);
    st.setValue(QStringLiteral("compress"), m_compress);
    st.setValue(QStringLiteral("ocr"), m_ocr);
    st.setValue(QStringLiteral("networkHost"), m_networkHost);
    if (!m_saveFolder.isEmpty())
        st.setValue(QStringLiteral("saveFolder"), m_saveFolder);
}
