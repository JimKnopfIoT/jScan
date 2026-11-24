#pragma once

// Lets the shared core (image processing, PDF, eSCL, SANE) build with the
// desktop's Qt 6 and with Qt 5.6 on Sailfish OS.
#include <QtGlobal>

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
typedef int qsizetype;
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#define JSCAN_SKIP_EMPTY Qt::SkipEmptyParts
#else
#define JSCAN_SKIP_EMPTY QString::SkipEmptyParts
#endif
