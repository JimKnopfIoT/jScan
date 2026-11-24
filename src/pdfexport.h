#pragma once

#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

struct PdfOptions {
    bool compress = true;   // JPEG for colour/grey pages, lossless otherwise
    bool ocr = true;        // invisible text layer via Tesseract
    int jpegQuality = 75;
};

namespace PdfExport {

// Path of the tesseract executable, or empty if not installed.
QString tesseractPath();
// OCR language string such as "deu+eng" from what is installed (empty if none).
QString ocrLanguages();

// Writes all pages into one PDF. Blocking – call from a worker thread.
// progress(text) is called from that thread.
// Returns an empty string on success, otherwise an error message.
// *ocrSkipped is set when OCR was requested but is not available.
QString write(const QList<QImage> &pages, const QString &file, const PdfOptions &opt,
              const std::function<void(const QString &)> &progress, bool *ocrSkipped);

} // namespace PdfExport
