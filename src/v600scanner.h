#pragma once

// Open driver for the Epson Perfection V600 Photo (USB 04b8:013a), talking to
// the scanner directly via libusb. Epson only offers a closed x86-64 plugin
// for it; this driver was written from USB captures of the scanner's normal
// operation and runs on any architecture (Linux desktop, Sailfish OS).
//
// The scanner needs Epson's firmware file esfwA1.bin, which is uploaded after
// every power-on. It is not part of jScan; see README ("Epson Perfection
// V600 Photo") for where to get it and where to put it.
//
// Currently supported: flatbed, color and grayscale, 400 dpi optical
// (lower resolutions are scaled down on the host).

#include <QImage>
#include <QList>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

namespace V600 {

// Device name used in ScanDevice/ScanSettings.
QString deviceName();
bool handles(const QString &device);

// True if a V600 is connected (does not open it).
bool isPresent();

// Firmware: locations searched, in this order, and the first one that exists.
QStringList firmwareSearchPaths();
QString firmwarePath();
// What is missing and where it has to go, for an info window.
QString firmwareMissingTitle();
QString firmwareMissingText();
// Error text returned by scan() when the firmware is needed but missing.
QString firmwareMissingError();

// Largest scan area (the glass) in mm.
QSizeF glassMm();
QList<int> resolutions();

struct Callbacks {
    std::function<void(const QString &)> status;
    // part of the page received so far (scaled down) and the expected full
    // height in the same scale
    std::function<void(const QImage &, int)> progress;
};

// 16 bit per sample needs Qt >= 5.13 (not on Sailfish OS).
bool supports16Bit();

// Scans `areaMm` (relative to the top left corner of the glass), depth 8 or
// 16 bit per sample (same tone curve, 16 bit = finer steps). Returns a null
// image and sets *error on failure or cancel.
QImage scan(const QRectF &areaMm, bool gray, int dpi, int depth, const std::atomic_bool &cancel,
            const Callbacks &cb, QString *error);

} // namespace V600
