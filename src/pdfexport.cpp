#include "pdfexport.h"

#include <QCoreApplication>
#include "compat.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>

namespace {

int imageDpi(const QImage &img)
{
    const int dpi = int(std::lround(img.dotsPerMeterX() * 0.0254));
    return dpi > 0 ? dpi : 300;
}

bool isMono(const QImage &img)
{
    return img.format() == QImage::Format_Mono || img.format() == QImage::Format_MonoLSB;
}

// Image prepared for the PDF: pixel format chosen, mono normalised so that
// bit 1 = black.
QImage prepare(const QImage &src, bool /*compress*/)
{
    if (isMono(src)) {
        QImage m = src.convertToFormat(QImage::Format_Mono);
        // make index 1 black
        if (m.colorCount() == 2 && qGray(m.color(1)) > qGray(m.color(0))) {
            m.invertPixels();
            m.setColorTable({m.color(1), m.color(0)});
        }
        return m;
    }
    if (src.format() == QImage::Format_Grayscale8 || src.isGrayscale())
        return src.convertToFormat(QImage::Format_Grayscale8);
#if QT_VERSION >= QT_VERSION_CHECK(5, 13, 0)
    if (src.format() == QImage::Format_Grayscale16)
        return src.convertToFormat(QImage::Format_Grayscale8);
#endif
    return src.convertToFormat(QImage::Format_RGB888);
}

QByteArray flate(const QByteArray &raw)
{
    QByteArray z = qCompress(raw, 9);
    return z.mid(4);   // strip Qt's 4-byte length prefix -> plain zlib stream
}

QByteArray rawRows(const QImage &img)
{
    int rowBytes = 0;
    switch (img.format()) {
    case QImage::Format_Mono:       rowBytes = (img.width() + 7) / 8; break;
    case QImage::Format_Grayscale8: rowBytes = img.width(); break;
    default:                        rowBytes = img.width() * 3; break;
    }
    QByteArray out;
    out.reserve(qsizetype(rowBytes) * img.height());
    for (int y = 0; y < img.height(); ++y)
        out.append(reinterpret_cast<const char *>(img.constScanLine(y)), rowBytes);
    return out;
}

// --- minimal PDF writer: one full-page image per page --------------------------
class PdfWriter
{
public:
    explicit PdfWriter(QIODevice *dev) : m_dev(dev) { write("%PDF-1.5\n%\xE2\xE3\xCF\xD3\n"); }

    void addPage(const QImage &img, bool compress, int jpegQuality)
    {
        const int dpi = imageDpi(img);
        const double wPt = img.width() * 72.0 / dpi, hPt = img.height() * 72.0 / dpi;

        QByteArray data, dict;
        dict = QByteArray("/Type /XObject /Subtype /Image /Width ")
               + QByteArray::number(img.width()) + " /Height "
               + QByteArray::number(img.height());
        if (img.format() == QImage::Format_Mono) {
            // PDF DeviceGray 1 bit: 0 = black. Our data: 1 = black -> Decode [1 0].
            dict += " /ColorSpace /DeviceGray /BitsPerComponent 1 /Decode [1 0]"
                    " /Filter /FlateDecode";
            data = flate(rawRows(img));
        } else {
            const bool grey = img.format() == QImage::Format_Grayscale8;
            dict += grey ? " /ColorSpace /DeviceGray" : " /ColorSpace /DeviceRGB";
            dict += " /BitsPerComponent 8";
            if (compress) {
                QBuffer buf(&data);
                buf.open(QIODevice::WriteOnly);
                img.save(&buf, "JPEG", jpegQuality);
                dict += " /Filter /DCTDecode";
            } else {
                data = flate(rawRows(img));
                dict += " /Filter /FlateDecode";
            }
        }
        const int imgObj = addStream(dict, data);

        const QByteArray content = "q " + num(wPt) + " 0 0 " + num(hPt) + " 0 0 cm /Im0 Do Q\n";
        const int contentObj = addStream({}, content);

        m_pageObjs << reserve();
        m_pending << QByteArray("<< /Type /Page /Parent %PAGES% 0 R /MediaBox [0 0 ")
                         + num(wPt) + " " + num(hPt) + "] /Resources << /XObject << /Im0 "
                         + QByteArray::number(imgObj) + " 0 R >> >> /Contents "
                         + QByteArray::number(contentObj) + " 0 R >>";
    }

    bool finish(const QString &title)
    {
        const int pagesObj = reserve();
        for (int i = 0; i < m_pageObjs.size(); ++i) {
            QByteArray body = m_pending[i];
            body.replace("%PAGES%", QByteArray::number(pagesObj));
            writeObj(m_pageObjs[i], body);
        }
        QByteArray kids;
        for (int id : m_pageObjs)
            kids += QByteArray::number(id) + " 0 R ";
        writeObj(pagesObj, "<< /Type /Pages /Kids [" + kids + "] /Count "
                               + QByteArray::number(m_pageObjs.size()) + " >>");
        const int infoObj = reserve();
        writeObj(infoObj, "<< /Title " + pdfString(title)
                              + " /Producer (jScan) >>");
        const int catalogObj = reserve();
        writeObj(catalogObj, "<< /Type /Catalog /Pages " + QByteArray::number(pagesObj)
                                 + " 0 R >>");

        const qint64 xref = m_pos;
        QByteArray x = "xref\n0 " + QByteArray::number(m_offsets.size() + 1)
                       + "\n0000000000 65535 f \n";
        for (qint64 off : m_offsets)
            x += QByteArray::number(off).rightJustified(10, '0') + " 00000 n \n";
        x += "trailer\n<< /Size " + QByteArray::number(m_offsets.size() + 1) + " /Root "
             + QByteArray::number(catalogObj) + " 0 R /Info " + QByteArray::number(infoObj)
             + " 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        write(x);
        return m_ok;
    }

private:
    static QByteArray num(double v) { return QByteArray::number(v, 'f', 2); }

    static QByteArray pdfString(const QString &s)
    {
        // UTF-16BE with BOM, as hex string: safe for umlauts and brackets
        QByteArray hex = "<FEFF";
        for (QChar c : s)
            hex += QByteArray::number(c.unicode(), 16).rightJustified(4, '0').toUpper();
        return hex + ">";
    }

    int reserve()
    {
        m_offsets << -1;
        return int(m_offsets.size());
    }

    void writeObj(int id, const QByteArray &body)
    {
        m_offsets[id - 1] = m_pos;
        write(QByteArray::number(id) + " 0 obj\n" + body + "\nendobj\n");
    }

    int addStream(const QByteArray &dict, const QByteArray &data)
    {
        const int id = reserve();
        m_offsets[id - 1] = m_pos;
        write(QByteArray::number(id) + " 0 obj\n<< " + dict + " /Length "
              + QByteArray::number(data.size()) + " >>\nstream\n");
        write(data);
        write("\nendstream\nendobj\n");
        return id;
    }

    void write(const QByteArray &b)
    {
        if (m_dev->write(b) != b.size())
            m_ok = false;
        m_pos += b.size();
    }

    QIODevice *m_dev;
    qint64 m_pos = 0;
    bool m_ok = true;
    QList<qint64> m_offsets;   // index = object id - 1
    QList<int> m_pageObjs;
    QList<QByteArray> m_pending;
};

QString writePlain(const QList<QImage> &pages, const QString &file, const PdfOptions &opt,
                   const std::function<void(const QString &)> &progress)
{
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly))
        return QCoreApplication::translate("jScan", "Cannot write the file: %1").arg(out.errorString());
    PdfWriter pdf(&out);
    for (int i = 0; i < pages.size(); ++i) {
        progress(QCoreApplication::translate("jScan", "Creating PDF – page %1 of %2 …").arg(i + 1).arg(pages.size()));
        pdf.addPage(prepare(pages[i], opt.compress), opt.compress, opt.jpegQuality);
    }
    if (!pdf.finish(QFileInfo(file).completeBaseName()) || !out.commit())
        return QCoreApplication::translate("jScan", "Write error: %1").arg(out.errorString());
    return {};
}

// Tesseract renders image + invisible text layer itself. JPEG input is
// embedded unchanged, so the compression choice is kept.
QString writeOcr(const QList<QImage> &pages, const QString &file, const PdfOptions &opt,
                 const std::function<void(const QString &)> &progress)
{
    QTemporaryDir tmp;
    if (!tmp.isValid())
        return QCoreApplication::translate("jScan", "Temporary directory not available.");
    // QTemporaryDir::filePath() needs Qt 5.9
    auto tmpPath = [&tmp](const QString &name) { return tmp.path() + QLatin1Char('/') + name; };

    QStringList list;
    for (int i = 0; i < pages.size(); ++i) {
        progress(QCoreApplication::translate("jScan", "Preparing text recognition – page %1 of %2 …")
                     .arg(i + 1).arg(pages.size()));
        const QImage img = prepare(pages[i], opt.compress);
        const bool jpeg = opt.compress && img.format() != QImage::Format_Mono;
        const QString path = tmpPath(QStringLiteral("p%1.%2")
                                              .arg(i, 4, 10, QLatin1Char('0'))
                                              .arg(jpeg ? QStringLiteral("jpg")
                                                        : QStringLiteral("png")));
        if (!img.save(path, jpeg ? "JPEG" : "PNG", jpeg ? opt.jpegQuality : -1))
            return QCoreApplication::translate("jScan", "Could not save an intermediate image.");
        list << path;
    }
    const QString listFile = tmpPath(QStringLiteral("pages.txt"));
    {
        QFile f(listFile);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return QCoreApplication::translate("jScan", "Could not write the page list.");
        f.write(list.join(QLatin1Char('\n')).toUtf8() + '\n');
    }

    const QString outBase = tmpPath(QStringLiteral("out"));
    QStringList args{listFile, outBase};
    const QString langs = PdfExport::ocrLanguages();
    if (!langs.isEmpty())
        args << QStringLiteral("-l") << langs;
    args << QStringLiteral("--dpi") << QString::number(imageDpi(pages.first()))
         // non-JPEG input is re-encoded by Tesseract; keep it near-lossless
         << QStringLiteral("-c")
         << QStringLiteral("jpg_quality=%1").arg(opt.compress ? opt.jpegQuality : 95)
         << QStringLiteral("pdf");

    progress(pages.size() == 1 ? QCoreApplication::translate("jScan", "Running text recognition …")
                               : QCoreApplication::translate("jScan", "Running text recognition (%1 pages) …").arg(pages.size()));
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(PdfExport::tesseractPath(), args);
    if (!proc.waitForStarted(10000))
        return QCoreApplication::translate("jScan", "Could not start Tesseract.");
    proc.waitForFinished(-1);
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return QCoreApplication::translate("jScan", "Text recognition failed:\n%1")
            .arg(QString::fromLocal8Bit(proc.readAll()).trimmed().right(600));

    const QString produced = outBase + QStringLiteral(".pdf");
    QFile::remove(file);
    if (!QFile::copy(produced, file))
        return QCoreApplication::translate("jScan", "Could not copy the PDF to %1.").arg(file);
    return {};
}

} // namespace

namespace PdfExport {

QString tesseractPath()
{
    QString p = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
#ifdef Q_OS_WIN
    if (p.isEmpty()) {
        const QStringList dirs{QStringLiteral("C:/Program Files/Tesseract-OCR"),
                               QStringLiteral("C:/Program Files (x86)/Tesseract-OCR")};
        p = QStandardPaths::findExecutable(QStringLiteral("tesseract"), dirs);
    }
#endif
    return p;
}

QString ocrLanguages()
{
    static QString cached;
    static bool done = false;
    if (done)
        return cached;
    done = true;
    const QString exe = tesseractPath();
    if (exe.isEmpty())
        return {};
    QProcess proc;
    proc.start(exe, {QStringLiteral("--list-langs")});
    if (!proc.waitForFinished(10000))
        return {};
    QStringList have;
    const auto lines = QString::fromLocal8Bit(proc.readAllStandardOutput())
                           .split(QLatin1Char('\n'), JSCAN_SKIP_EMPTY);
    for (const QString &l : lines) {
        const QString t = l.trimmed();
        if (!t.isEmpty() && !t.contains(QLatin1Char(' ')) && !t.endsWith(QLatin1Char(':'))
            && t != QLatin1String("osd"))
            have << t;
    }
    QStringList use;
    for (const char *pref : {"deu", "eng"})
        if (have.contains(QLatin1String(pref)))
            use << QLatin1String(pref);
    if (use.isEmpty() && !have.isEmpty())
        use << have.first();
    cached = use.join(QLatin1Char('+'));
    return cached;
}

QString write(const QList<QImage> &pages, const QString &file, const PdfOptions &opt,
              const std::function<void(const QString &)> &progress, bool *ocrSkipped)
{
    if (ocrSkipped)
        *ocrSkipped = false;
    if (pages.isEmpty())
        return QCoreApplication::translate("jScan", "No pages.");
    if (opt.ocr) {
        if (!tesseractPath().isEmpty() && !ocrLanguages().isEmpty())
            return writeOcr(pages, file, opt, progress);
        if (ocrSkipped)
            *ocrSkipped = true;
    }
    return writePlain(pages, file, opt, progress);
}

} // namespace PdfExport
