#pragma once

#include "imageops.h"
#include "scanworker.h"

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QQuickImageProvider>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QVariantList>

// Scanner, pages and saving for the QML UI. Same behaviour as the desktop
// MainWindow: raw pages are kept, the visible page is raw + page settings,
// so contrast, background, deskew and colour can be changed after scanning.
class ScanController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(int deviceIndex READ deviceIndex WRITE setDeviceIndex NOTIFY deviceIndexChanged)
    Q_PROPERTY(QStringList sources READ sources NOTIFY capsChanged)
    Q_PROPERTY(int sourceIndex READ sourceIndex WRITE setSourceIndex NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList resolutions READ resolutions NOTIFY capsChanged)
    Q_PROPERTY(int dpiIndex READ dpiIndex WRITE setDpiIndex NOTIFY settingsChanged)
    Q_PROPERTY(bool feeder READ feeder NOTIFY settingsChanged)
    Q_PROPERTY(QString feedHint READ feedHint NOTIFY capsChanged)
    Q_PROPERTY(bool ocr READ ocr WRITE setOcr NOTIFY settingsChanged)
    Q_PROPERTY(bool ocrAvailable READ ocrAvailable CONSTANT)
    Q_PROPERTY(int colorMode READ colorMode WRITE setColorMode NOTIFY pageSettingsChanged)
    Q_PROPERTY(int contrast READ contrast WRITE setContrast NOTIFY pageSettingsChanged)
    Q_PROPERTY(int flatten READ flatten WRITE setFlatten NOTIFY pageSettingsChanged)
    Q_PROPERTY(bool deskew READ deskew WRITE setDeskew NOTIFY pageSettingsChanged)
    Q_PROPERTY(bool skipBlank READ skipBlank WRITE setSkipBlank NOTIFY settingsChanged)
    Q_PROPERTY(bool compress READ compress WRITE setCompress NOTIFY settingsChanged)
    Q_PROPERTY(QString networkHost READ networkHost WRITE setNetworkHost NOTIFY settingsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY pagesChanged)
    Q_PROPERTY(int currentPage READ currentPage WRITE setCurrentPage NOTIFY currentPageChanged)
    Q_PROPERTY(QStringList thumbs READ thumbs NOTIFY pagesChanged)
    Q_PROPERTY(QString currentImage READ currentImage NOTIFY currentImageChanged)
    Q_PROPERTY(QString liveImage READ liveImage NOTIFY liveImageChanged)
    Q_PROPERTY(QString saveFolder READ saveFolder NOTIFY saveFolderChanged)

public:
    explicit ScanController(QObject *parent = nullptr);
    ~ScanController() override;

    QStringList devices() const;
    int deviceIndex() const { return m_deviceIndex; }
    void setDeviceIndex(int i);
    QStringList sources() const;
    int sourceIndex() const { return m_sourceIndex; }
    void setSourceIndex(int i);
    QVariantList resolutions() const;
    int dpiIndex() const { return m_dpiIndex; }
    void setDpiIndex(int i);
    bool feeder() const;
    QString feedHint() const;
    bool ocr() const { return m_ocr; }
    void setOcr(bool b);
    bool ocrAvailable() const;

    int colorMode() const { return int(m_ps.mode); }
    void setColorMode(int m);
    int contrast() const { return m_ps.contrast; }
    void setContrast(int c);
    int flatten() const { return m_ps.flatten; }
    void setFlatten(int f);
    bool deskew() const { return m_ps.deskew; }
    void setDeskew(bool d);
    bool skipBlank() const { return m_skipBlank; }
    void setSkipBlank(bool b);
    bool compress() const { return m_compress; }
    void setCompress(bool b);
    QString networkHost() const { return m_networkHost; }
    void setNetworkHost(const QString &h);

    QString status() const { return m_status; }
    bool busy() const { return m_state != Idle; }
    bool scanning() const { return m_state == Scanning; }
    bool ready() const { return m_state == Idle && !m_caps.device.isEmpty(); }
    int pageCount() const;
    int currentPage() const { return m_current; }
    void setCurrentPage(int i);
    QStringList thumbs() const;
    QString currentImage() const;
    QString liveImage() const { return m_liveUrl; }
    QString saveFolder() const;

    // for the image provider (any thread)
    QImage image(const QString &id, const QSize &requested);

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void scan();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void save(bool png);
    // Save dialog: suggested name (without extension), check for an existing
    // file, save under the chosen folder/name (the folder is remembered).
    Q_INVOKABLE QString defaultFileName() const;
    Q_INVOKABLE bool fileExists(const QString &folder, const QString &name, bool png) const;
    Q_INVOKABLE void saveAs(bool png, const QString &folder, const QString &name);
    Q_INVOKABLE void rotatePage(int index, int degrees);
    Q_INVOKABLE void removePage(int index);
    Q_INVOKABLE void movePage(int from, int to);
    Q_INVOKABLE void clearPages();

signals:
    void devicesChanged();
    void deviceIndexChanged();
    void capsChanged();
    void settingsChanged();
    void pageSettingsChanged();
    void statusChanged();
    void stateChanged();
    void pagesChanged();
    void currentPageChanged();
    void currentImageChanged();
    void liveImageChanged();
    void saved(const QString &path);
    void saveFolderChanged();
    // something the user has to do (e.g. missing firmware): shown on a page
    void notice(const QString &title, const QString &text);
    void error(const QString &message);

private slots:
    void onDevices(const QList<ScanDevice> &devices);
    void onCaps(const DeviceCaps &caps, const QString &error);
    void onPage(const QImage &img);
    void onProgress(const QImage &partial, int expectedHeight);
    void onFinished(int pages, const QString &error);
    void onProcessed(int id, quint64 gen, const QImage &img);
    void onSaveDone(const QStringList &files, const QString &error);
    void setStatus(const QString &s);
    void applyPageSettings();

private:
    enum State { Idle, Searching, Querying, Scanning, Saving };
    struct Page {
        int id = 0;
        QImage raw;
        QImage processed;
        ImageOps::PageSettings ps;
        quint64 gen = 0;
        bool dirty = true;
    };

    void setState(State s);
    void queryCaps();
    void processPage(int index);
    void loadSettings();
    void saveSettings() const;
    int indexOfId(int id) const;
    void pageSettingChanged();

    QThread m_thread;
    ScanWorker *m_worker = nullptr;
    State m_state = Idle;
    QString m_status;

    QList<ScanDevice> m_devices;
    int m_deviceIndex = -1;
    QString m_prefDevice;
    DeviceCaps m_caps;
    int m_sourceIndex = 0;
    int m_dpiIndex = 0;
    QString m_prefSource;
    int m_prefDpi = 300;

    ImageOps::PageSettings m_ps;   // shown in the controls; used for new pages
    bool m_skipBlank = true;
    bool m_compress = true;
    bool m_ocr = true;
    QString m_networkHost;
    QString m_saveFolder;

    mutable QMutex m_mutex;        // guards m_pages and m_live (image provider thread)
    QList<Page> m_pages;
    int m_current = -1;
    int m_nextId = 1;
    int m_skipped = 0;
    QImage m_live;
    QString m_liveUrl;
    int m_liveSerial = 0;
    QTimer m_reprocess;
};

class PageImageProvider : public QQuickImageProvider
{
public:
    explicit PageImageProvider(ScanController *c)
        : QQuickImageProvider(QQuickImageProvider::Image), m_c(c) {}
    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override
    {
        QImage img = m_c->image(id, requested);
        if (size)
            *size = img.size();
        return img;
    }

private:
    ScanController *m_c;
};
