#pragma once

#include <QColor>

class QApplication;

// Dark neon look shared with the harbour-pm5 / harbour-sysmetrics apps.
// Uses the Fusion style so the app looks identical on Linux and Windows.
namespace Theme {
inline const QColor Orange{0xff, 0x8a, 0x00};
inline const QColor Green{0x28, 0xc7, 0x6f};
inline const QColor Red{0xff, 0x4d, 0x4d};
inline const QColor Blue{0x4d, 0xb8, 0xff};
inline const QColor Text{0xe0, 0xe0, 0xe0};
inline const QColor TextDim{0x6b, 0x6f, 0x7a};
inline const QColor BgTop{0x1b, 0x20, 0x29};
inline const QColor BgMid{0x12, 0x16, 0x1d};
inline const QColor BgBottom{0x0b, 0x0e, 0x13};

void apply(QApplication &app);
} // namespace Theme
