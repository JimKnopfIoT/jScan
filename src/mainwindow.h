#pragma once

#include "scanworker.h"
#include "imageops.h"

#include <QHash>
#include <QMainWindow>
#include <QThread>

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QToolButton;
class QSlider;
class QTimer;
class PreviewWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *e) override;

private slots:
    void refreshDevices();
    void onDevices(const QList<ScanDevice> &devices);
    void onDeviceChanged();
    void onCaps(const DeviceCaps &caps, const QString &error);
    void startScan();
    void cancelScan();
    void quickScan();
    void onPage(const QImage &img);
    void onProgress(const QImage &partial, int expectedHeight);
    void onPreview(const QImage &img);
    void onCurrentPageChanged();
    void clearArea();
    void onFinished(int pages, const QString &error);
    void savePdf();
    void onSaved(const QStringList &files, const QString &error, bool ocrSkipped, bool png);
    void clearAll();
    void rotateSelected(int degrees);
    void deleteSelected();
    void showPreview(QListWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void onPageSettingsChanged();
    void onProcessed(int id, quint64 gen, const QImage &img);

private:
    enum class State { Idle, Searching, Querying, Scanning, Saving };

    struct Page {
        QImage raw;          // as delivered by the scanner (rotation applied)
        QImage processed;    // raw + ps; may lag behind while dirty
        ImageOps::PageSettings ps;
        quint64 gen = 0;
        bool dirty = true;
    };

    bool startSave();   // false when the user cancelled the file dialog
    void addPage(const QImage &img);
    ImageOps::PageSettings controlSettings() const;
    void showControlsFor(const ImageOps::PageSettings &ps);
    void processPage(int id);
    QImage pageImage(int id) const;
    int currentPageId() const;
    void setState(State s);
    void updatePaperAvailability();
    void updateSourceDependent();
    void loadFlattenPref();
    void beginScan(bool preview);
    void loadSettings();
    void saveSettings() const;
    static bool isBlank(const QImage &img);

    QComboBox *m_device = nullptr;
    QToolButton *m_refreshBtn = nullptr;
    QComboBox *m_source = nullptr;
    QComboBox *m_mode = nullptr;
    QComboBox *m_dpi = nullptr;
    QComboBox *m_paper = nullptr;
    QCheckBox *m_skipBlank = nullptr;
    QCheckBox *m_deskew = nullptr;
    QSlider *m_flatten = nullptr;
    QLabel *m_flattenValue = nullptr;
    QSlider *m_contrast = nullptr;
    QLabel *m_contrastValue = nullptr;
    QCheckBox *m_openAfter = nullptr;
    QCheckBox *m_compress = nullptr;
    QCheckBox *m_ocr = nullptr;
    QPushButton *m_quickBtn = nullptr;
    QPushButton *m_scanBtn = nullptr;
    QLabel *m_areaLabel = nullptr;
    QToolButton *m_areaClearBtn = nullptr;
    PreviewWidget *m_preview = nullptr;
    QPushButton *m_cancelBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_status = nullptr;

    QThread m_thread;
    ScanWorker *m_worker = nullptr;
    State m_state = State::Idle;
    DeviceCaps m_caps;          // valid when m_caps.device is non-empty
    QString m_capsDeviceKey;
    int m_skipped = 0;
    bool m_scanIsPreview = false;
    QRectF m_areaMm;   // flatbed scan area chosen in the quick scan
    QString m_flattenLoadedFor;   // source whose flatten preference is shown
    int m_nextId = 1;
    QHash<int, Page> m_pages;
    QTimer *m_reprocessTimer = nullptr;
    bool m_loadingControls = false;

    // remembered preferences
    QString m_lastDir;
    int m_lastSaveFormat = 0;   // 0 PDF, 1 PNG, 2 TIFF (unprocessed scan data)
    bool m_closeAfterSave = false;
    QString m_prefDevice, m_prefSource, m_prefMode;
    int m_prefDpi = 300;
    QString m_prefPaper;
};
