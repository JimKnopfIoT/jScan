#include "mainwindow.h"
#include "previewwidget.h"
#include "pdfexport.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
const QSize kThumb(110, 156);

struct Paper { const char *key; const char *label; double w, h; };
const Paper kPapers[] = {
    {"A4", "A4 (210 × 297 mm)", 210.0, 297.0},
    {"A5", "A5 (148 × 210 mm)", 148.0, 210.0},
    {"Letter", "Letter (216 × 279 mm)", 215.9, 279.4},
    {"Legal", "Legal (216 × 356 mm)", 215.9, 355.6},
    {"Max", "Gesamte Scanfläche", 0, 0},
};

QString sourceLabel(const QString &s)
{
    static const QHash<QString, QString> map{
        {QStringLiteral("ADF Duplex"), QStringLiteral("Einzug – beidseitig")},
        {QStringLiteral("ADF Front"), QStringLiteral("Einzug – nur Vorderseite")},
        {QStringLiteral("ADF Back"), QStringLiteral("Einzug – nur Rückseite")},
        {QStringLiteral("ADF"), QStringLiteral("Einzug")},
        {QStringLiteral("Automatic Document Feeder"), QStringLiteral("Einzug")},
        {QStringLiteral("Flatbed"), QStringLiteral("Vorlagenglas")},
        {QStringLiteral("Card Front"), QStringLiteral("Kartenschlitz – Vorderseite")},
        {QStringLiteral("Card Duplex"), QStringLiteral("Kartenschlitz – beidseitig")},
        {QStringLiteral("Transparency Unit"), QStringLiteral("Durchlicht (Dias/Negative)")},
        {QStringLiteral("TPU8x10"), QStringLiteral("Durchlicht 8×10\"")},
    };
    return map.value(s, s);
}

QString modeLabel(const QString &m)
{
    static const QHash<QString, QString> map{
        {QStringLiteral("Color"), QStringLiteral("Farbe")},
        {QStringLiteral("Gray"), QStringLiteral("Graustufen")},
        {QStringLiteral("Color16"), QStringLiteral("Farbe, 16 Bit")},
        {QStringLiteral("Gray16"), QStringLiteral("Graustufen, 16 Bit")},
        {QStringLiteral("Lineart"), QStringLiteral("Schwarzweiß")},
        {QStringLiteral("Binary"), QStringLiteral("Schwarzweiß")},
        {QStringLiteral("Halftone"), QStringLiteral("Raster")},
    };
    return map.value(m, m);
}

// Thumbnail icon that keeps its colours when selected (Fusion would tint it).
QIcon thumbIcon(const QImage &img)
{
    const QPixmap pm = QPixmap::fromImage(
        img.scaled(kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    QIcon icon;
    icon.addPixmap(pm, QIcon::Normal);
    icon.addPixmap(pm, QIcon::Selected);
    return icon;
}

int imageDpi(const QImage &img)
{
    const int dpi = int(std::lround(img.dotsPerMeterX() * 0.0254));
    return dpi > 0 ? dpi : 300;
}

QString deviceGroup(const QString &key)
{
    QString k = key;
    k.replace(QLatin1Char('/'), QLatin1Char('_'));
    return QStringLiteral("devices/") + k;
}

// "eine Seite" / "3 Seiten"
QString pagesText(int n)
{
    return n == 1 ? QStringLiteral("eine Seite") : QStringLiteral("%1 Seiten").arg(n);
}

void selectData(QComboBox *c, const QVariant &v)
{
    const int i = c->findData(v);
    if (i >= 0)
        c->setCurrentIndex(i);
}
} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("jScan"));

    // --- settings panel ---------------------------------------------------
    m_device = new QComboBox;
    m_device->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_device->setMinimumContentsLength(22);
    m_refreshBtn = new QToolButton;
    m_refreshBtn->setText(QStringLiteral("⟳"));
    m_refreshBtn->setToolTip(QStringLiteral("Scanner neu suchen"));
    auto *deviceRow = new QHBoxLayout;
    deviceRow->addWidget(m_device, 1);
    deviceRow->addWidget(m_refreshBtn);

    m_source = new QComboBox;
    m_mode = new QComboBox;
    m_dpi = new QComboBox;
    m_paper = new QComboBox;
    for (const Paper &p : kPapers)
        m_paper->addItem(QString::fromUtf8(p.label), QString::fromLatin1(p.key));

    m_skipBlank = new QCheckBox(QStringLiteral("Leere Seiten entfernen"));
    m_deskew = new QCheckBox(QStringLiteral("Geraderichten"));
    m_deskew->setToolTip(QStringLiteral("Schief eingezogene oder aufgelegte Seiten "
                                        "automatisch gerade ausrichten.\n"
                                        "Wirkt auf die ausgewählten Seiten und die nächsten Scans."));
    m_flatten = new QSlider(Qt::Horizontal);
    m_flatten->setRange(0, 100);
    m_flatten->setPageStep(10);
    m_flattenValue = new QLabel;
    m_flattenValue->setMinimumWidth(34);
    m_flattenValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *flattenRow = new QHBoxLayout;
    flattenRow->addWidget(m_flatten, 1);
    flattenRow->addWidget(m_flattenValue);
    connect(m_flatten, &QSlider::valueChanged, this, [this](int v) {
        m_flattenValue->setText(v == 0 ? QStringLiteral("aus") : QString::number(v));
    });
    m_flatten->setToolTip(QStringLiteral(
        "Hintergrund glätten: gleicht den Papieruntergrund aus – Schatten von Faltungen,\n"
        "ungleichmäßige Ausleuchtung, Vergilbung und Farbstich werden weiß.\n"
        "Höhere Werte machen auch hellgraue Reste (Knicklinien, Durchschein) weiß;\n"
        "sehr heller Text kann dann mit verschwinden. 0 = aus.\n"
        "Für Dokumente gedacht – bei Fotos ausschalten.\n"
        "Wirkt auf die ausgewählten Seiten und die nächsten Scans;\n"
        "die Voreinstellung wird pro Scanner und Vorlage gespeichert."));
    m_contrast = new QSlider(Qt::Horizontal);
    m_contrast->setRange(-50, 50);
    m_contrast->setPageStep(10);
    m_contrast->setToolTip(QStringLiteral(
        "Papier bleibt weiß, Text wird dunkler.\n"
        "Wirkt auf die ausgewählten Seiten und die nächsten Scans."));
    m_contrastValue = new QLabel;
    m_contrastValue->setMinimumWidth(34);
    m_contrastValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *contrastRow = new QHBoxLayout;
    contrastRow->addWidget(m_contrast, 1);
    contrastRow->addWidget(m_contrastValue);
    connect(m_contrast, &QSlider::valueChanged, this, [this](int v) {
        m_contrastValue->setText(v > 0 ? QStringLiteral("+%1").arg(v) : QString::number(v));
    });
    m_openAfter = new QCheckBox(QStringLiteral("Nach dem Speichern öffnen"));
    m_compress = new QCheckBox(QStringLiteral("Komprimieren"));
    m_compress->setToolTip(QStringLiteral(
        "Farb- und Graustufenseiten als JPEG – deutlich kleinere Dateien.\n"
        "Aus: verlustfrei. Für reine Textseiten lohnt „Graustufen“ als Farbmodus."));
    m_ocr = new QCheckBox(QStringLiteral("Durchsuchbar (Texterkennung)"));
    m_ocr->setToolTip(QStringLiteral(
        "Legt eine unsichtbare Textebene über die Seiten (Tesseract OCR).\n"
        "Text lässt sich dann suchen und kopieren."));

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Scanner:"), deviceRow);
    form->addRow(QStringLiteral("Vorlage:"), m_source);
    form->addRow(QStringLiteral("Farbe:"), m_mode);
    form->addRow(QStringLiteral("Auflösung:"), m_dpi);
    form->addRow(QStringLiteral("Papier:"), m_paper);
    form->addRow(QStringLiteral("Kontrast:"), contrastRow);
    form->addRow(QStringLiteral("Glätten:"), flattenRow);
    form->addRow(m_deskew);
    form->addRow(m_skipBlank);
    auto *settingsBox = new QGroupBox(QStringLiteral("Scan"));
    settingsBox->setLayout(form);

    auto *pdfLay = new QVBoxLayout;
    pdfLay->addWidget(m_compress);
    pdfLay->addWidget(m_ocr);
    pdfLay->addWidget(m_openAfter);
    auto *pdfBox = new QGroupBox(QStringLiteral("PDF"));
    pdfBox->setLayout(pdfLay);

    m_quickBtn = new QPushButton(QStringLiteral("Schnellscan"));
    m_quickBtn->setObjectName(QStringLiteral("quickBtn"));
    m_quickBtn->setMinimumHeight(44);
    m_quickBtn->setToolTip(QStringLiteral(
        "Schnelle Übersicht des Vorlagenglases.\n"
        "Darin den gewünschten Bereich mit der Maus aufziehen."));

    m_areaLabel = new QLabel;
    m_areaLabel->setObjectName(QStringLiteral("hint"));
    m_areaClearBtn = new QToolButton;
    m_areaClearBtn->setText(QStringLiteral("✕"));
    m_areaClearBtn->setToolTip(QStringLiteral("Bereich aufheben – ganzes Papierformat scannen"));
    auto *areaRow = new QHBoxLayout;
    areaRow->addWidget(m_areaLabel, 1);
    areaRow->addWidget(m_areaClearBtn);

    m_scanBtn = new QPushButton(QStringLiteral("Scannen"));
    m_scanBtn->setObjectName(QStringLiteral("scanBtn"));
    m_scanBtn->setMinimumHeight(64);
    QFont f = m_scanBtn->font();
    f.setPointSizeF(f.pointSizeF() * 1.4);
    f.setBold(true);
    m_scanBtn->setFont(f);
    m_scanBtn->setShortcut(QKeySequence(Qt::Key_F5));
    m_scanBtn->setToolTip(QStringLiteral("Vorlage einlegen und scannen (F5).\n"
                                         "Weitere Scans werden angehängt."));

    m_cancelBtn = new QPushButton(QStringLiteral("Abbrechen"));
    m_cancelBtn->setObjectName(QStringLiteral("cancelBtn"));
    m_cancelBtn->setMinimumHeight(64);
    m_cancelBtn->setFont(f);
    m_cancelBtn->setVisible(false);

    m_saveBtn = new QPushButton(QStringLiteral("Speichern …"));
    m_saveBtn->setToolTip(QStringLiteral("Als PDF oder als PNG-Bilder speichern (Strg+S)"));
    m_saveBtn->setObjectName(QStringLiteral("saveBtn"));
    m_saveBtn->setMinimumHeight(44);
    m_saveBtn->setShortcut(QKeySequence::Save);

    m_clearBtn = new QPushButton(QStringLiteral("Alle Seiten verwerfen"));
    m_clearBtn->setObjectName(QStringLiteral("clearBtn"));

    auto *left = new QVBoxLayout;
    left->addWidget(settingsBox);
    left->addWidget(pdfBox);
    left->addSpacing(8);
    left->addWidget(m_quickBtn);
    left->addLayout(areaRow);
    left->addWidget(m_scanBtn);
    left->addWidget(m_cancelBtn);
    left->addStretch();
    left->addWidget(m_saveBtn);
    left->addWidget(m_clearBtn);

    // --- page list ----------------------------------------------------------
    m_preview = new PreviewWidget;

    m_list = new QListWidget;
    m_list->setViewMode(QListView::IconMode);
    m_list->setFlow(QListView::TopToBottom);
    m_list->setWrapping(false);
    m_list->setIconSize(kThumb);
    m_list->setGridSize(kThumb + QSize(24, 30));
    m_list->setResizeMode(QListView::Adjust);
    m_list->setMovement(QListView::Snap);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFixedWidth(kThumb.width() + 56);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setDefaultDropAction(Qt::MoveAction);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setWordWrap(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);

    auto *hint = new QLabel(QStringLiteral(
        "Strg+Rad: zoomen · Ziehen: verschieben · Doppelklick: einpassen   |   "
        "Kontrast, Glätten, Geraderichten wirken auf die rechts gewählten Seiten   |   "
        "Seiten ziehen zum Sortieren, Rechtsklick zum Drehen/Löschen"));
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);

    auto *center = new QVBoxLayout;
    center->addWidget(m_preview, 1);
    center->addWidget(hint);

    auto *central = new QWidget;
    central->setObjectName(QStringLiteral("central"));
    auto *main = new QHBoxLayout(central);
    main->addLayout(left);
    main->addLayout(center, 1);
    main->addWidget(m_list);
    setCentralWidget(central);

    m_status = new QLabel;
    statusBar()->addWidget(m_status, 1);

    // --- actions -------------------------------------------------------------
    auto *del = new QAction(this);
    del->setShortcut(QKeySequence::Delete);
    del->setShortcutContext(Qt::WidgetShortcut);
    m_list->addAction(del);
    connect(del, &QAction::triggered, this, &MainWindow::deleteSelected);

    connect(m_refreshBtn, &QToolButton::clicked, this, &MainWindow::refreshDevices);
    connect(m_device, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceChanged);
    connect(m_scanBtn, &QPushButton::clicked, this, &MainWindow::startScan);
    connect(m_quickBtn, &QPushButton::clicked, this, &MainWindow::quickScan);
    connect(m_areaClearBtn, &QToolButton::clicked, this, &MainWindow::clearArea);
    connect(m_preview, &PreviewWidget::selectionChanged, this, [this](const QRectF &mm) {
        m_areaMm = mm;
        updateSourceDependent();
    });
    connect(m_list, &QListWidget::currentItemChanged, this, &MainWindow::onCurrentPageChanged);
    connect(m_list, &QListWidget::itemClicked, this, &MainWindow::onCurrentPageChanged);
    connect(m_cancelBtn, &QPushButton::clicked, this, &MainWindow::cancelScan);
    connect(m_saveBtn, &QPushButton::clicked, this, &MainWindow::savePdf);
    connect(m_clearBtn, &QPushButton::clicked, this, &MainWindow::clearAll);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &MainWindow::showPreview);
    connect(m_list, &QWidget::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(m_list->model(), &QAbstractItemModel::rowsMoved, this, [this] {
        for (int i = 0; i < m_list->count(); ++i)
            m_list->item(i)->setText(QStringLiteral("Seite %1").arg(i + 1));
    });
    // page settings: apply to the selected pages (slider debounced)
    m_reprocessTimer = new QTimer(this);
    m_reprocessTimer->setSingleShot(true);
    m_reprocessTimer->setInterval(150);
    connect(m_reprocessTimer, &QTimer::timeout, this, &MainWindow::onPageSettingsChanged);
    for (QSlider *sl : {m_contrast, m_flatten})
        connect(sl, &QSlider::valueChanged, this, [this] {
            if (!m_loadingControls)
                m_reprocessTimer->start();
        });
    connect(m_deskew, &QCheckBox::toggled, this, [this] {
        if (!m_loadingControls)
            onPageSettingsChanged();
    });
    connect(m_mode, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_loadingControls)
            onPageSettingsChanged();
    });
    m_mode->setToolTip(QStringLiteral(
        "Farbmodus für den nächsten Scan und für die ausgewählten Seiten.\n"
        "Farbig gescannte Seiten lassen sich nachträglich auf Graustufen oder\n"
        "Schwarzweiß stellen (und zurück); grau gescannte bleiben grau."));

    connect(m_source, &QComboBox::currentIndexChanged, this, [this] {
        m_areaMm = {};
        updateSourceDependent();
        updatePaperAvailability();
        setState(m_state);
    });

    // --- worker thread ------------------------------------------------------
    m_worker = new ScanWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &ScanWorker::status, m_status, &QLabel::setText);
    connect(m_worker, &ScanWorker::devicesFound, this, &MainWindow::onDevices);
    connect(m_worker, &ScanWorker::capsReady, this, &MainWindow::onCaps);
    connect(m_worker, &ScanWorker::pageScanned, this, &MainWindow::onPage);
    connect(m_worker, &ScanWorker::pageProgress, this, &MainWindow::onProgress);
    connect(m_worker, &ScanWorker::previewReady, this, &MainWindow::onPreview);
    connect(m_worker, &ScanWorker::finished, this, &MainWindow::onFinished);
    connect(m_worker, &ScanWorker::notice, this, [this](const QString &title, const QString &text) {
        QMessageBox box(QMessageBox::Information, title, text, QMessageBox::Ok, this);
        box.setTextInteractionFlags(Qt::TextSelectableByMouse);   // paths can be copied
        box.exec();
    });
    m_thread.start();

    loadSettings();
    resize(1050, 720);
    refreshDevices();
}

MainWindow::~MainWindow()
{
    QThreadPool::globalInstance()->waitForDone();   // page processing uses `this`
    m_worker->requestCancel();
    m_thread.quit();
    m_thread.wait();
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    if (m_state == State::Saving) {
        e->ignore();
        return;
    }
    if (m_state == State::Scanning) {
        m_worker->requestCancel();
        e->ignore();
        m_status->setText(QStringLiteral("Scan wird abgebrochen …"));
        return;
    }
    if (m_list->count() > 0) {
        const int n = m_list->count();
        QMessageBox box(QMessageBox::Warning, windowTitle(),
                        n == 1 ? QStringLiteral("Eine Seite ist noch nicht gespeichert.")
                               : QStringLiteral("%1 Seiten sind noch nicht gespeichert.").arg(n),
                        QMessageBox::NoButton, this);
        box.setInformativeText(n == 1
                                   ? QStringLiteral("Ohne Speichern geht sie beim Beenden verloren.")
                                   : QStringLiteral("Ohne Speichern gehen sie beim Beenden verloren."));
        QPushButton *save = box.addButton(QStringLiteral("Speichern …"), QMessageBox::AcceptRole);
        QPushButton *discard =
            box.addButton(QStringLiteral("Nicht speichern"), QMessageBox::DestructiveRole);
        QPushButton *back = box.addButton(QStringLiteral("Zurück"), QMessageBox::RejectRole);
        // the theme's button padding is not fully included in QMessageBox's layout
        for (QAbstractButton *b : box.buttons())
            b->setMinimumWidth(b->fontMetrics().horizontalAdvance(b->text()) + 48);
        box.setDefaultButton(back);
        box.setEscapeButton(back);
        box.exec();
        if (box.clickedButton() == save) {
            e->ignore();
            m_closeAfterSave = startSave();   // closes by itself once saved
            return;
        }
        if (box.clickedButton() != discard) {
            e->ignore();
            return;
        }
    }
    saveSettings();
    e->accept();
}

// --- device handling ---------------------------------------------------------

void MainWindow::refreshDevices()
{
    if (m_state == State::Scanning || m_state == State::Searching)
        return;
    setState(State::Searching);
    QMetaObject::invokeMethod(m_worker, &ScanWorker::listDevices, Qt::QueuedConnection);
}

void MainWindow::onDevices(const QList<ScanDevice> &devices)
{
    if (!m_capsDeviceKey.isEmpty())
        m_prefDevice = m_capsDeviceKey;

    {
        QSignalBlocker block(m_device);
        m_device->clear();
        for (const ScanDevice &d : devices) {
            m_device->addItem(d.label, d.name);
            m_device->setItemData(m_device->count() - 1, d.key, Qt::UserRole + 1);
            m_device->setItemData(m_device->count() - 1, d.name, Qt::ToolTipRole);
        }
        int idx = m_device->findData(m_prefDevice, Qt::UserRole + 1);
        if (idx < 0) {   // prefer a sheet-feed Canon if nothing remembered
            for (int i = 0; i < m_device->count(); ++i)
                if (m_device->itemData(i).toString().startsWith(QLatin1String("canon_dr")))
                    idx = i;
        }
        m_device->setCurrentIndex(std::max(0, idx));
    }

    if (devices.isEmpty()) {
        m_caps = {};
        setState(State::Idle);
        m_status->setText(QStringLiteral("Kein Scanner gefunden. Gerät einschalten/anstecken "
                                         "und auf ⟳ klicken."));
        return;
    }
    onDeviceChanged();
}

void MainWindow::onDeviceChanged()
{
    // remember the settings of the device we are leaving
    if (!m_capsDeviceKey.isEmpty())
        saveSettings();

    m_caps = {};
    m_capsDeviceKey.clear();
    m_areaMm = {};
    const QString name = m_device->currentData().toString();
    if (name.isEmpty()) {
        setState(State::Idle);
        return;
    }
    setState(State::Querying);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, name] { w->queryCaps(name); },
                              Qt::QueuedConnection);
}

void MainWindow::onCaps(const DeviceCaps &caps, const QString &error)
{
    if (caps.device != m_device->currentData().toString())
        return;   // answer for a device that is no longer selected

    if (!error.isEmpty()) {
        m_caps = {};
        setState(State::Idle);
        m_status->setText(error);
        return;
    }
    m_caps = caps;
    m_capsDeviceKey = m_device->currentData(Qt::UserRole + 1).toString();

    // per-device preferences
    QSettings st;
    st.beginGroup(deviceGroup(m_capsDeviceKey));
    const QString prefSource = st.value(QStringLiteral("source")).toString();
    const QString prefMode = st.value(QStringLiteral("mode"), QStringLiteral("Color")).toString();
    const int prefDpi = st.value(QStringLiteral("dpi"), 300).toInt();
    const QString prefPaper = st.value(QStringLiteral("paper"), QStringLiteral("A4")).toString();
    st.endGroup();
    m_flattenLoadedFor.clear();

    {
        QSignalBlocker b1(m_source), b2(m_mode), b3(m_dpi);
        m_source->clear();
        for (const QString &s : caps.sources)
            m_source->addItem(sourceLabel(s), s);
        m_source->setEnabled(m_source->count() > 1);
        if (m_source->findData(prefSource) >= 0)
            selectData(m_source, prefSource);
        else
            selectData(m_source, QStringLiteral("ADF Duplex"));

        m_mode->clear();
        for (const QString &m : caps.modes)
            m_mode->addItem(modeLabel(m), m);
        selectData(m_mode, prefMode);

        m_dpi->clear();
        for (int r : caps.resolutions)
            m_dpi->addItem(QStringLiteral("%1 dpi").arg(r), r);
        int best = 0;
        for (int i = 0; i < m_dpi->count(); ++i)
            if (std::abs(m_dpi->itemData(i).toInt() - prefDpi)
                < std::abs(m_dpi->itemData(best).toInt() - prefDpi))
                best = i;
        m_dpi->setCurrentIndex(best);
    }
    updateSourceDependent();
    selectData(m_paper, prefPaper);
    updatePaperAvailability();

    setState(State::Idle);
    m_status->setText(QStringLiteral("Bereit: %1. Vorlage einlegen und „Scannen“ drücken.")
                          .arg(m_device->currentText()));
}

void MainWindow::updatePaperAvailability()
{
    auto *model = qobject_cast<QStandardItemModel *>(m_paper->model());
    const bool feeder = ScanWorker::isFeeder(m_source->currentData().toString());
    const QSizeF max = !feeder && !m_caps.flatbedMm.isEmpty() ? m_caps.flatbedMm : m_caps.maxMm;
    for (int i = 0; i < m_paper->count(); ++i) {
        const Paper &p = kPapers[i];
        const bool fits = p.w == 0 || max.isEmpty()
                          || (p.w <= max.width() + 1.0 && p.h <= max.height() + 1.0);
        if (model)
            model->item(i)->setEnabled(fits);
    }
    if (model && !model->item(m_paper->currentIndex())->isEnabled())
        m_paper->setCurrentIndex(0);
}

// --- scanning ------------------------------------------------------------------

void MainWindow::startScan()
{
    beginScan(false);
}

void MainWindow::quickScan()
{
    beginScan(true);
}

void MainWindow::beginScan(bool preview)
{
    if (m_state != State::Idle || m_caps.device.isEmpty())
        return;
    ScanSettings s;
    s.preview = preview;
    s.deskew = m_deskew->isChecked();
    s.device = m_caps.device;
    s.source = m_source->currentData().toString();
    s.mode = m_mode->currentData().toString();
    s.dpi = m_dpi->currentData().toInt();
    const Paper &p = kPapers[std::clamp(m_paper->currentIndex(), 0, int(std::size(kPapers)) - 1)];
    const QSizeF glass = !m_caps.flatbedMm.isEmpty() ? m_caps.flatbedMm : m_caps.maxMm;
    s.paperMm = p.w > 0 ? QSizeF(p.w, p.h)
                        : (ScanWorker::isFeeder(s.source) ? m_caps.maxMm : glass);
    if (!ScanWorker::isFeeder(s.source))
        s.areaMm = m_areaMm;

    m_skipped = 0;
    m_scanIsPreview = preview;
    m_preview->setPlaceholder(preview ? QStringLiteral("Schnellscan läuft …")
                                      : QStringLiteral("Scan läuft …"));
    m_preview->clear();
    saveSettings();
    setState(State::Scanning);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, s] { w->scan(s); },
                              Qt::QueuedConnection);
}

void MainWindow::cancelScan()
{
    m_worker->requestCancel();
    m_status->setText(QStringLiteral("Wird abgebrochen …"));
}

void MainWindow::onPage(const QImage &img)
{
    if (m_skipBlank->isEnabled() && m_skipBlank->isChecked() && isBlank(img)) {
        ++m_skipped;
        return;
    }
    addPage(img);
}

void MainWindow::onProgress(const QImage &partial, int expectedHeight)
{
    m_preview->setLiveImage(partial, expectedHeight);
}

void MainWindow::onPreview(const QImage &raw)
{
    ImageOps::PageSettings ps = controlSettings();
    ps.deskew = false;   // the selection must match the glass geometry
    const QImage img = ImageOps::process(raw, ps);
    const double mmPerPx = 25.4 / imageDpi(img);
    m_preview->setImage(img, true, mmPerPx);
    m_areaMm = {};
    m_list->setCurrentItem(nullptr);
    updateSourceDependent();
    m_status->setText(QStringLiteral("Schnellscan fertig. Bereich mit der Maus aufziehen "
                                     "(Rechtsklick hebt ihn auf), dann „Scannen“."));
}

void MainWindow::onCurrentPageChanged()
{
    const int id = currentPageId();
    if (!m_pages.contains(id))
        return;
    showControlsFor(m_pages[id].ps);
    m_preview->setImage(pageImage(id));
}

int MainWindow::currentPageId() const
{
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(kIdRole).toInt() : -1;
}

QImage MainWindow::pageImage(int id) const
{
    const Page &p = m_pages[id];
    return p.processed.isNull() ? p.raw : p.processed;
}

ImageOps::PageSettings MainWindow::controlSettings() const
{
    ImageOps::PageSettings ps;
    ps.flatten = m_flatten->value();
    ps.deskew = m_deskew->isChecked();
    ps.contrast = m_contrast->value();
    const QString m = m_mode->currentData().toString();
    ps.mode = m == QLatin1String("Gray")      ? ImageOps::ColorMode::Gray
              : m == QLatin1String("Lineart") ? ImageOps::ColorMode::Lineart
                                              : ImageOps::ColorMode::Color;
    return ps;
}

void MainWindow::showControlsFor(const ImageOps::PageSettings &ps)
{
    m_loadingControls = true;
    m_flatten->setValue(ps.flatten);
    m_deskew->setChecked(ps.deskew);
    m_contrast->setValue(ps.contrast);
    selectData(m_mode, ps.mode == ImageOps::ColorMode::Gray      ? QStringLiteral("Gray")
                       : ps.mode == ImageOps::ColorMode::Lineart ? QStringLiteral("Lineart")
                                                                 : QStringLiteral("Color"));
    m_loadingControls = false;
}

// A control changed: re-process every selected page with the new settings.
void MainWindow::onPageSettingsChanged()
{
    const ImageOps::PageSettings ps = controlSettings();
    for (QListWidgetItem *item : m_list->selectedItems()) {
        const int id = item->data(kIdRole).toInt();
        if (!m_pages.contains(id) || m_pages[id].ps == ps)
            continue;
        m_pages[id].ps = ps;
        processPage(id);
    }
}

// Runs raw + settings in the thread pool; stale results are dropped by gen.
void MainWindow::processPage(int id)
{
    Page &p = m_pages[id];
    p.dirty = true;
    const quint64 gen = ++p.gen;
    const QImage raw = p.raw;
    const ImageOps::PageSettings ps = p.ps;
    QThreadPool::globalInstance()->start([this, id, gen, raw, ps] {
        const QImage out = ImageOps::process(raw, ps);
        QMetaObject::invokeMethod(this, [this, id, gen, out] { onProcessed(id, gen, out); });
    });
}

void MainWindow::onProcessed(int id, quint64 gen, const QImage &img)
{
    auto it = m_pages.find(id);
    if (it == m_pages.end() || it->gen != gen)
        return;
    it->processed = img;
    it->dirty = false;
    for (int i = 0; i < m_list->count(); ++i)
        if (m_list->item(i)->data(kIdRole).toInt() == id)
            m_list->item(i)->setIcon(thumbIcon(img));
    if (currentPageId() == id)
        m_preview->replaceImage(img);
}

void MainWindow::clearArea()
{
    m_areaMm = {};
    m_preview->clearSelection();
    updateSourceDependent();
}

// "Hintergrund glätten" is remembered per scanner and source: on for the
// sheet feeder (documents), off for flatbeds (could be photos).
void MainWindow::loadFlattenPref()
{
    const QString source = m_source->currentData().toString();
    if (m_capsDeviceKey.isEmpty() || source.isEmpty() || source == m_flattenLoadedFor)
        return;
    QSettings st;
    st.beginGroup(deviceGroup(m_capsDeviceKey));
    if (!m_flattenLoadedFor.isEmpty())   // remember the choice for the old source
        st.setValue(QStringLiteral("flattenStrength/") + m_flattenLoadedFor, m_flatten->value());
    m_loadingControls = true;
    m_flatten->setValue(st.value(QStringLiteral("flattenStrength/") + source,
                                 ScanWorker::isFeeder(source) ? 50 : 0).toInt());
    m_loadingControls = false;
    st.endGroup();
    m_flattenLoadedFor = source;
}

void MainWindow::updateSourceDependent()
{
    const bool feeder = ScanWorker::isFeeder(m_source->currentData().toString());
    const bool known = !m_caps.device.isEmpty();
    // Blank-page removal only makes sense for the sheet feeder; the quick scan
    // only for the flatbed (a feeder would eject the sheet).
    m_skipBlank->setEnabled(feeder);
    m_quickBtn->setVisible(known && !feeder);
    loadFlattenPref();
    const bool area = known && !feeder && !m_areaMm.isEmpty();
    m_areaLabel->setVisible(known && !feeder);
    m_areaClearBtn->setVisible(area);
    m_areaLabel->setText(area ? QStringLiteral("Bereich: %1 × %2 mm")
                                    .arg(qRound(m_areaMm.width()))
                                    .arg(qRound(m_areaMm.height()))
                              : QStringLiteral("Bereich: ganzes Papierformat"));
    if (m_list->count() == 0 && m_state != State::Scanning)
        m_preview->setPlaceholder(
            !known ? QStringLiteral("Kein Scanner bereit.")
            : feeder ? (m_caps.device.startsWith(QLatin1String("canon_dr"))
                            ? QStringLiteral("Blätter mit der Vorderseite nach unten und kopfüber "
                                             "in den Einzug legen und „Scannen“ drücken.")
                            : QStringLiteral("Blätter in den Einzug legen und „Scannen“ drücken."))
                           + QStringLiteral("\n\nDie Seiten erscheinen hier live während des Scans.")
                     : QStringLiteral("Vorlage auflegen.\n\n„Schnellscan“ zeigt eine "
                                      "Übersicht, in der du den Bereich aufziehen kannst. "
                                      "„Scannen“ erfasst dann nur diesen Bereich."));
}

void MainWindow::onFinished(int pages, const QString &error)
{
    setState(State::Idle);
    if (m_scanIsPreview && error.isEmpty())
        return;   // onPreview already updated the status line
    if (!error.isEmpty() && pages == 0) {
        m_status->setText(error);
        if (error != QCoreApplication::translate("jScan", "Cancelled."))
            QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    QString msg = pagesText(pages) + QStringLiteral(" gescannt");
    msg[0] = msg[0].toUpper();
    if (m_skipped > 0)
        msg += m_skipped == 1 ? QStringLiteral(", eine leere entfernt")
                              : QStringLiteral(", %1 leere entfernt").arg(m_skipped);
    msg += QStringLiteral(" – insgesamt %1 im Dokument.").arg(pagesText(m_list->count()));
    if (!error.isEmpty())
        msg += QStringLiteral("  (%1)").arg(error);
    m_status->setText(msg);
}

void MainWindow::addPage(const QImage &img)
{
    const int id = m_nextId++;
    Page page;
    page.raw = img;
    page.ps = controlSettings();
    m_pages.insert(id, page);
    auto *item = new QListWidgetItem(thumbIcon(img),
                                     QStringLiteral("Seite %1").arg(m_list->count() + 1));
    item->setData(kIdRole, id);
    item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
    m_list->addItem(item);
    m_list->scrollToItem(item);
    m_list->setCurrentItem(item, QItemSelectionModel::ClearAndSelect);
    m_preview->setImage(img);
    processPage(id);
    setState(m_state);
}

void MainWindow::setState(State s)
{
    m_state = s;
    const bool scanning = s == State::Scanning;
    const bool busy = scanning || s == State::Saving;
    const bool idle = s == State::Idle;
    const bool ready = idle && !m_caps.device.isEmpty();

    m_scanBtn->setVisible(!scanning);
    m_cancelBtn->setVisible(scanning);
    m_scanBtn->setEnabled(ready);
    m_quickBtn->setEnabled(ready);
    m_device->setEnabled(idle && m_device->count() > 0);
    m_refreshBtn->setEnabled(idle);
    for (QWidget *w : std::initializer_list<QWidget *>{m_dpi, m_paper})
        w->setEnabled(ready);
    m_source->setEnabled(ready && m_source->count() > 1);

    const bool havePages = m_list->count() > 0;
    m_saveBtn->setEnabled(havePages && !busy);
    m_clearBtn->setEnabled(havePages && !busy);
    m_list->setEnabled(!busy || scanning);
}

// --- saving ----------------------------------------------------------------------

void MainWindow::savePdf()
{
    startSave();
}

bool MainWindow::startSave()
{
    if (m_list->count() == 0)
        return false;

    QString dir = m_lastDir;
    if (dir.isEmpty() || !QDir(dir).exists()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
              + QStringLiteral("/Scans");
        QDir().mkpath(dir);
    }
    const QString pdfFilter = QStringLiteral("PDF-Dokument (*.pdf)");
    const QString pngFilter = QStringLiteral(
        "PNG-Bilder – eine Datei pro Seite, ohne Texterkennung und PDF-Komprimierung (*.png)");
    const QString tiffFilter = QStringLiteral(
        "TIFF – unbearbeitete Scandaten (16 Bit, wenn so gescannt), eine Datei pro Seite, "
        "z. B. für darktable (*.tif *.tiff)");
    const QStringList filters{pdfFilter, pngFilter, tiffFilter};
    static const char *const exts[] = {".pdf", ".png", ".tif"};
    const int lastFormat = std::clamp(m_lastSaveFormat, 0, 2);
    QString selected = filters[lastFormat];
    const QString suggested = dir + QStringLiteral("/Scan_")
                              + QDateTime::currentDateTime().toString(
                                  QStringLiteral("yyyy-MM-dd_HH-mm-ss"))
                              + QLatin1String(exts[lastFormat]);
    QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Speichern"), suggested,
                                                filters.join(QStringLiteral(";;")), &selected);
    if (file.isEmpty())
        return false;
    // the typed extension wins over the filter
    int format = std::max(0, int(filters.indexOf(selected)));
    if (file.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        format = 1;
    else if (file.endsWith(QStringLiteral(".pdf"), Qt::CaseInsensitive))
        format = 0;
    else if (file.endsWith(QStringLiteral(".tif"), Qt::CaseInsensitive)
             || file.endsWith(QStringLiteral(".tiff"), Qt::CaseInsensitive))
        format = 2;
    else
        file += QLatin1String(exts[format]);
    m_lastDir = QFileInfo(file).absolutePath();
    m_lastSaveFormat = format;
    const bool png = format == 1;
    const bool tiff = format == 2;

    QList<QImage> pages;
    QList<int> dirtyIdx;
    QList<ImageOps::PageSettings> dirtyPs;
    QList<QImage> dirtyRaw;
    for (int i = 0; i < m_list->count(); ++i) {
        const Page &p = m_pages[m_list->item(i)->data(kIdRole).toInt()];
        if (tiff) {   // unprocessed scan data in full depth (rotation applied)
            pages << p.raw;
            continue;
        }
        pages << p.processed;
        if (p.dirty || p.processed.isNull()) {
            dirtyIdx << i;
            dirtyRaw << p.raw;
            dirtyPs << p.ps;
        }
    }

    // PNG/TIFF: one file per page; several pages get a running number
    QStringList targets;
    if (png || tiff) {
        const QFileInfo fi(file);
        const QString base = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName();
        const int digits = pages.size() >= 100 ? 3 : 2;
        for (int i = 0; i < pages.size(); ++i)
            targets << (pages.size() == 1
                            ? file
                            : QStringLiteral("%1_%2%3")
                                  .arg(base)
                                  .arg(i + 1, digits, 10, QLatin1Char('0'))
                                  .arg(QLatin1Char('.') + fi.suffix()));
        QStringList existing;
        for (const QString &t : targets)
            if (t != file && QFileInfo::exists(t))
                existing << QFileInfo(t).fileName();
        if (!existing.isEmpty()
            && QMessageBox::question(this, windowTitle(),
                                     QStringLiteral("Diese Dateien gibt es schon und würden "
                                                    "überschrieben:\n%1\n\nFortfahren?")
                                         .arg(existing.join(QLatin1Char('\n'))))
                   != QMessageBox::Yes)
            return false;
    } else {
        targets << file;
    }

    PdfOptions opt;
    opt.compress = m_compress->isChecked();
    opt.ocr = m_ocr->isChecked();
    saveSettings();
    setState(State::Saving);
    m_status->setText(tiff  ? QStringLiteral("Speichere TIFF …")
                      : png ? QStringLiteral("Speichere PNG …")
                            : QStringLiteral("Speichere PDF …"));

    QThread *t = QThread::create([this, pages, targets, png, tiff, opt, dirtyIdx, dirtyRaw,
                                  dirtyPs]() mutable {
        for (int k = 0; k < dirtyIdx.size(); ++k)   // pages still being processed
            pages[dirtyIdx[k]] = ImageOps::process(dirtyRaw[k], dirtyPs[k]);
        auto progress = [this](const QString &msg) {
            QMetaObject::invokeMethod(m_status, [this, msg] { m_status->setText(msg); });
        };
        bool ocrSkipped = false;
        QString err;
        if (png || tiff) {
            const char *fmt = tiff ? "TIFF" : "PNG";
            for (int i = 0; i < pages.size() && err.isEmpty(); ++i) {
                progress(QStringLiteral("Speichere %1 %2 von %3 …")
                             .arg(QLatin1String(fmt))
                             .arg(i + 1)
                             .arg(pages.size()));
                if (!pages[i].save(targets[i], fmt))
                    err = QStringLiteral("Die Datei konnte nicht geschrieben werden:\n%1")
                              .arg(targets[i]);
            }
        } else {
            err = PdfExport::write(pages, targets.first(), opt, progress, &ocrSkipped);
        }
        QMetaObject::invokeMethod(this, [this, targets, err, ocrSkipped, png, tiff] {
            onSaved(targets, err, ocrSkipped, png || tiff);
        });
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
    return true;
}

void MainWindow::onSaved(const QStringList &files, const QString &error, bool ocrSkipped,
                         bool png)
{
    setState(State::Idle);
    const bool closeNow = m_closeAfterSave && error.isEmpty();
    m_closeAfterSave = false;
    if (!error.isEmpty()) {
        m_status->setText(QStringLiteral("Fehler beim Speichern."));
        QMessageBox::critical(this, windowTitle(), error);
        return;
    }

    qint64 bytes = 0;
    for (const QString &f : files)
        bytes += QFileInfo(f).size();
    const double mb = bytes / 1048576.0;
    if (png && files.size() > 1)
        m_status->setText(QStringLiteral("Gespeichert: %1 %2-Dateien in %3 (%4 MB)")
                              .arg(files.size())
                              .arg(QFileInfo(files.first()).suffix().toUpper())
                              .arg(QFileInfo(files.first()).absolutePath())
                              .arg(mb, 0, 'f', 1));
    else
        m_status->setText(QStringLiteral("Gespeichert: %1 (%2, %3 MB)")
                              .arg(files.first(), pagesText(m_list->count()))
                              .arg(mb, 0, 'f', 1));
    if (ocrSkipped) {
#ifdef Q_OS_WIN
        const QString how = QStringLiteral("Tesseract für Windows installieren "
                                           "(github.com/UB-Mannheim/tesseract).");
#else
        const QString how = QStringLiteral(
            "Installieren mit:\n  sudo pacman -S tesseract tesseract-data-deu "
            "tesseract-data-eng");
#endif
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("Das PDF wurde ohne Texterkennung gespeichert, "
                                                "weil Tesseract nicht gefunden wurde.\n\n%1")
                                     .arg(how));
    }
    if (m_openAfter->isChecked()) {
        // several PNGs: open the folder, otherwise the file itself
        const QString target = files.size() > 1 ? QFileInfo(files.first()).absolutePath()
                                                : files.first();
        QDesktopServices::openUrl(QUrl::fromLocalFile(target));
    }

    if (closeNow) {   // "Speichern …" chosen while quitting
        m_list->clear();
        m_pages.clear();
        close();
        return;
    }

    if (QMessageBox::question(this, windowTitle(),
                              QStringLiteral("%1 gespeichert.\nSeiten für ein neues Dokument "
                                             "verwerfen?")
                                  .arg(png ? QStringLiteral("Bilder") : QStringLiteral("PDF")))
        == QMessageBox::Yes) {
        m_list->clear();
        m_pages.clear();
        m_preview->clear();
        updateSourceDependent();
        setState(m_state);
    }
}

void MainWindow::clearAll()
{
    if (QMessageBox::question(this, windowTitle(),
                              m_list->count() == 1
                                  ? QStringLiteral("Die Seite verwerfen?")
                                  : QStringLiteral("Alle %1 Seiten verwerfen?").arg(m_list->count()))
        != QMessageBox::Yes)
        return;
    m_list->clear();
    m_pages.clear();
    m_preview->clear();
    updateSourceDependent();
    setState(m_state);
    m_status->setText(QStringLiteral("Bereit."));
}

// --- page editing ------------------------------------------------------------------

void MainWindow::rotateSelected(int degrees)
{
    for (QListWidgetItem *item : m_list->selectedItems()) {
        const int id = item->data(kIdRole).toInt();
        Page &p = m_pages[id];
        const QTransform t = QTransform().rotate(degrees);
        p.raw = p.raw.transformed(t);
        if (!p.processed.isNull())
            p.processed = p.processed.transformed(t);
        item->setIcon(thumbIcon(pageImage(id)));
        if (p.dirty)   // a result for the old orientation may be on its way
            processPage(id);
    }
    onCurrentPageChanged();
}

void MainWindow::deleteSelected()
{
    const auto items = m_list->selectedItems();
    for (QListWidgetItem *item : items) {
        m_pages.remove(item->data(kIdRole).toInt());
        delete item;
    }
    for (int i = 0; i < m_list->count(); ++i)
        m_list->item(i)->setText(QStringLiteral("Seite %1").arg(i + 1));
    if (m_list->count() == 0) {
        m_preview->clear();
        updateSourceDependent();
    } else {
        onCurrentPageChanged();
    }
    setState(m_state);
}

void MainWindow::showPreview(QListWidgetItem *item)
{
    if (!item)
        return;
    const QImage img = pageImage(item->data(kIdRole).toInt());
    QDialog dlg(this);
    dlg.setWindowTitle(item->text());
    auto *label = new QLabel;
    const int h = std::max(400, int(screen()->availableGeometry().height() * 0.85));
    label->setPixmap(QPixmap::fromImage(
        img.scaledToHeight(std::min(h, img.height()), Qt::SmoothTransformation)));
    auto *area = new QScrollArea;
    area->setWidget(label);
    area->setAlignment(Qt::AlignCenter);
    auto *lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(area);
    dlg.resize(label->pixmap().width() + 30, h + 10);
    dlg.exec();
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item)
        return;
    if (!item->isSelected()) {
        m_list->clearSelection();
        item->setSelected(true);
    }
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(QStringLiteral("zoom-in")), QStringLiteral("Ansehen"),
                   this, [this, item] { showPreview(item); });
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("object-rotate-left")),
                   QStringLiteral("Nach links drehen"), this, [this] { rotateSelected(-90); });
    menu.addAction(QIcon::fromTheme(QStringLiteral("object-rotate-right")),
                   QStringLiteral("Nach rechts drehen"), this, [this] { rotateSelected(90); });
    menu.addAction(QStringLiteral("Um 180° drehen"), this, [this] { rotateSelected(180); });
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(QStringLiteral("edit-delete")), QStringLiteral("Löschen"),
                   this, &MainWindow::deleteSelected);
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

bool MainWindow::isBlank(const QImage &img)
{
    return ImageOps::isBlank(img);
}

// --- settings ------------------------------------------------------------------------

void MainWindow::loadSettings()
{
    QSettings st;
    m_prefDevice = st.value(QStringLiteral("device")).toString();
    m_skipBlank->setChecked(st.value(QStringLiteral("skipBlank"), true).toBool());
    m_openAfter->setChecked(st.value(QStringLiteral("openAfter"), true).toBool());
    m_compress->setChecked(st.value(QStringLiteral("compress"), true).toBool());
    m_loadingControls = true;
    m_deskew->setChecked(st.value(QStringLiteral("deskew"), true).toBool());
    m_contrast->setValue(st.value(QStringLiteral("contrast"), 0).toInt());
    m_loadingControls = false;
    m_contrastValue->setText(m_contrast->value() > 0
                                 ? QStringLiteral("+%1").arg(m_contrast->value())
                                 : QString::number(m_contrast->value()));
    m_ocr->setChecked(st.value(QStringLiteral("ocr"), true).toBool());
    m_lastDir = st.value(QStringLiteral("lastDir")).toString();
    m_lastSaveFormat = st.value(QStringLiteral("saveFormat"),
                                st.value(QStringLiteral("savePng"), false).toBool() ? 1 : 0)
                           .toInt();
    restoreGeometry(st.value(QStringLiteral("geometry")).toByteArray());
}

void MainWindow::saveSettings() const
{
    QSettings st;
    if (!m_capsDeviceKey.isEmpty()) {
        st.setValue(QStringLiteral("device"), m_capsDeviceKey);
        st.beginGroup(deviceGroup(m_capsDeviceKey));
        st.setValue(QStringLiteral("source"), m_source->currentData());
        st.setValue(QStringLiteral("mode"), m_mode->currentData());
        st.setValue(QStringLiteral("dpi"), m_dpi->currentData());
        st.setValue(QStringLiteral("paper"), m_paper->currentData());
        if (!m_flattenLoadedFor.isEmpty())
            st.setValue(QStringLiteral("flattenStrength/") + m_flattenLoadedFor, m_flatten->value());
        st.endGroup();
    }
    st.setValue(QStringLiteral("skipBlank"), m_skipBlank->isChecked());
    st.setValue(QStringLiteral("openAfter"), m_openAfter->isChecked());
    st.setValue(QStringLiteral("compress"), m_compress->isChecked());
    st.setValue(QStringLiteral("deskew"), m_deskew->isChecked());
    st.setValue(QStringLiteral("contrast"), m_contrast->value());
    st.setValue(QStringLiteral("ocr"), m_ocr->isChecked());
    st.setValue(QStringLiteral("lastDir"), m_lastDir);
    st.setValue(QStringLiteral("saveFormat"), m_lastSaveFormat);
    st.setValue(QStringLiteral("geometry"), saveGeometry());
}
