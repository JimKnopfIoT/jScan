#include "theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace Theme {

namespace {
QString rgba(const QColor &c, double alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(c.red()).arg(c.green()).arg(c.blue()).arg(int(alpha * 255));
}

// A neon tile button in the accent colour (like ActionTile.qml).
QString tile(const char *id, const QColor &accent)
{
    return QStringLiteral(
               "QPushButton#%1 { color: %2; background: %3; border: 1px solid %4;"
               "  border-radius: 14px; padding: 8px 14px; }"
               "QPushButton#%1:hover { background: %5; border-color: %6; }"
               "QPushButton#%1:pressed { background: %7; border-color: %8; }"
               "QPushButton#%1:disabled { color: %9; border-color: %10; background: %11; }")
        .arg(QLatin1String(id), accent.name(), rgba(Qt::white, 0.04), rgba(accent, 0.45),
             rgba(accent, 0.10), rgba(accent, 0.75), rgba(accent, 0.22), accent.name(),
             rgba(Text, 0.25))
        .arg(rgba(accent, 0.15), rgba(Qt::white, 0.02));
}
} // namespace

void apply(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    p.setColor(QPalette::Window, BgMid);
    p.setColor(QPalette::WindowText, Text);
    p.setColor(QPalette::Base, QColor(0x0f, 0x12, 0x18));
    p.setColor(QPalette::AlternateBase, BgTop);
    p.setColor(QPalette::Text, Text);
    p.setColor(QPalette::Button, BgTop);
    p.setColor(QPalette::ButtonText, Text);
    p.setColor(QPalette::BrightText, Red);
    p.setColor(QPalette::Highlight, Orange);
    p.setColor(QPalette::HighlightedText, BgBottom);
    p.setColor(QPalette::ToolTipBase, BgTop);
    p.setColor(QPalette::ToolTipText, Text);
    p.setColor(QPalette::PlaceholderText, TextDim);
    p.setColor(QPalette::Link, Blue);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, TextDim);
    app.setPalette(p);

    QString css = QStringLiteral(R"(
        QMainWindow, QDialog {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 %1, stop:0.6 %2, stop:1 %3);
        }
        QWidget#central { background: transparent; }
        QToolTip { color: %4; background: %1; border: 1px solid %5; padding: 4px; }

        QGroupBox {
            background: %6; border: 1px solid %7; border-radius: 14px;
            margin-top: 22px; padding: 14px 10px 10px 10px;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 14px; padding: 0 4px;
            color: %8; font-weight: bold; letter-spacing: 1px;
        }
        QLabel { color: %4; background: transparent; }
        QLabel#hint { color: %9; }

        QComboBox {
            color: %4; background: %10; border: 1px solid %11;
            border-radius: 8px; padding: 5px 10px; min-height: 22px;
        }
        QComboBox:hover { border-color: %12; }
        QComboBox:disabled { color: %9; border-color: %13; }
        QComboBox::drop-down { border: none; width: 22px; }
        QComboBox QAbstractItemView {
            color: %4; background: %1; border: 1px solid %12;
            selection-background-color: %14; selection-color: %4; outline: 0;
        }

        QCheckBox { color: %4; spacing: 8px; }
        QCheckBox:disabled { color: %9; }
        QCheckBox::indicator {
            width: 16px; height: 16px; border-radius: 4px;
            border: 1px solid %11; background: %10;
        }
        QCheckBox::indicator:checked { background: %8; border-color: %8; }

        QSlider::groove:horizontal { height: 4px; border-radius: 2px; background: %11; }
        QSlider::sub-page:horizontal { border-radius: 2px; background: %12; }
        QSlider::handle:horizontal {
            width: 14px; height: 14px; margin: -6px 0; border-radius: 7px;
            background: %8; border: 2px solid %3;
        }
        QSlider::handle:horizontal:disabled { background: %9; }
        QSlider::sub-page:horizontal:disabled { background: %13; }

        QToolButton {
            background: %6; border: 1px solid %11; border-radius: 8px; padding: 4px;
        }
        QToolButton:hover { border-color: %12; background: %15; }

        QListWidget {
            background: rgba(0,0,0,60); border: 1px solid %7; border-radius: 14px;
            padding: 8px; color: %4;
        }
        QListWidget::item { color: %4; border-radius: 8px; padding: 4px; }
        QListWidget::item:hover { background: %15; }
        QListWidget::item:selected { background: %14; color: %8;
            border: 1px solid %12; }

        QScrollArea { background: transparent; border: none; }
        QScrollBar:vertical, QScrollBar:horizontal { background: transparent; margin: 0; }
        QScrollBar:vertical { width: 10px; }
        QScrollBar:horizontal { height: 10px; }
        QScrollBar::handle { background: %11; border-radius: 5px; min-height: 30px;
            min-width: 30px; }
        QScrollBar::handle:hover { background: %12; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

        QStatusBar { background: %3; color: %4; border-top: 1px solid %7; }
        QStatusBar QLabel { padding: 2px 6px; }

        QMenu { background: %1; color: %4; border: 1px solid %11; padding: 4px; }
        QMenu::item { padding: 6px 20px; border-radius: 6px; }
        QMenu::item:selected { background: %14; color: %8; }
        QMenu::separator { height: 1px; background: %7; margin: 4px 8px; }

        QMessageBox QPushButton, QFileDialog QPushButton {
            color: %4; background: %6; border: 1px solid %11; border-radius: 8px;
            padding: 6px 16px; min-width: 70px;
        }
        QMessageBox QPushButton:hover, QFileDialog QPushButton:hover { border-color: %12; }
    )")
        .arg(BgTop.name(), BgMid.name(), BgBottom.name(), Text.name(),
             rgba(Orange, 0.45), rgba(Qt::white, 0.04), rgba(Qt::white, 0.08), Orange.name(),
             TextDim.name())
        .arg(rgba(Qt::white, 0.05), rgba(Qt::white, 0.14), rgba(Orange, 0.75),
             rgba(Qt::white, 0.06), rgba(Orange, 0.18), rgba(Orange, 0.08));

    css += tile("scanBtn", Orange);
    css += tile("quickBtn", Blue);
    css += tile("saveBtn", Green);
    css += tile("clearBtn", Red);
    css += tile("cancelBtn", Red);
    app.setStyleSheet(css);
}

} // namespace Theme
