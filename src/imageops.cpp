#include "imageops.h"

#include <QPainter>
#include <QPointF>
#include <QTransform>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace ImageOps {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxAngle = 5.0;    // degrees searched in each direction
constexpr double kMinAngle = 0.1;    // below this the page is left alone

// Projection-profile score: when the rotation lines the text up with the
// rows, the row histogram becomes "spiky" and the sum of squares peaks.
double score(const QVector<QPointF> &pts, double deg, int rows)
{
    const double a = deg * kPi / 180.0;
    const double s = std::sin(a), c = std::cos(a);
    QVector<int> bins(rows * 2, 0);
    for (const QPointF &p : pts) {
        const int y = int(std::lround(p.y() * c - p.x() * s)) + rows / 2;
        if (y >= 0 && y < bins.size())
            ++bins[y];
    }
    double sum = 0;
    for (int b : bins)
        sum += double(b) * b;
    return sum;
}
} // namespace

double detectSkew(const QImage &img)
{
    if (img.isNull())
        return 0;
    // work on a small grey copy: fast and enough for 0.05° resolution
    const int w = std::min(img.width(), 1000);
    const QImage g = img.convertToFormat(QImage::Format_Grayscale8)
                         .scaledToWidth(w, Qt::SmoothTransformation);
    const int mx = g.width() / 20, my = g.height() / 20;   // skip scanner edges
    QVector<QPointF> pts;
    for (int y = my; y < g.height() - my; ++y) {
        const uchar *line = g.constScanLine(y);
        for (int x = mx; x < g.width() - mx; ++x)
            if (line[x] < 140)
                pts << QPointF(x - g.width() / 2.0, y - g.height() / 2.0);
    }
    if (pts.size() < 300)   // (nearly) blank page: nothing to align to
        return 0;
    if (pts.size() > 60000) {   // dense page: thin out evenly
        QVector<QPointF> thin;
        const int step = int(pts.size() / 60000) + 1;
        for (int i = 0; i < pts.size(); i += step)
            thin << pts[i];
        pts.swap(thin);
    }

    const int rows = g.height();
    auto best = [&](double from, double to, double step) {
        double bestA = 0, bestS = -1;
        for (double a = from; a <= to + 1e-9; a += step) {
            const double sc = score(pts, a, rows);
            if (sc > bestS) {
                bestS = sc;
                bestA = a;
            }
        }
        return bestA;
    };
    const double coarse = best(-kMaxAngle, kMaxAngle, 0.5);
    const double fine = best(coarse - 0.5, coarse + 0.5, 0.05);
    // the edge of the search range means "no clear answer"
    if (std::abs(fine) >= kMaxAngle - 0.01)
        return 0;
    return fine;
}

namespace {
// Separable running max / box mean on one 8-bit plane (small images only).
void maxFilter(QVector<uchar> &p, int w, int h, int r)
{
    QVector<uchar> tmp(p.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uchar m = 0;
            for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k)
                m = std::max(m, p[y * w + k]);
            tmp[y * w + x] = m;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uchar m = 0;
            for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k)
                m = std::max(m, tmp[k * w + x]);
            p[y * w + x] = m;
        }
}

void boxBlur(QVector<uchar> &p, int w, int h, int r)
{
    QVector<uchar> tmp(p.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int s = 0, n = 0;
            for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k, ++n)
                s += p[y * w + k];
            tmp[y * w + x] = uchar(s / n);
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int s = 0, n = 0;
            for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k, ++n)
                s += tmp[k * w + x];
            p[y * w + x] = uchar(s / n);
        }
}
} // namespace

QImage flattenBackground(const QImage &img, int strength)
{
    strength = std::clamp(strength, 0, 100);
    if (strength == 0 || img.isNull() || img.format() == QImage::Format_Mono
        || img.format() == QImage::Format_MonoLSB)
        return img;
    const bool grey = img.format() == QImage::Format_Grayscale8;
    QImage out = img.convertToFormat(grey ? QImage::Format_Grayscale8 : QImage::Format_RGB888);
    const int ch = grey ? 1 : 3;

    // 1. background estimate on a ~40 dpi copy: averaging thins the text,
    //    a small max filter removes what is left of it.
    int dpi = int(std::lround(img.dotsPerMeterX() * 0.0254));
    if (dpi <= 0)
        dpi = 300;
    const double factor = std::max(1.0, dpi / 40.0);
    const int sw = std::max(8, int(out.width() / factor));
    const int sh = std::max(8, int(out.height() / factor));
    const QImage small = out.scaled(sw, sh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int n = sw * sh;

    QVector<QVector<uchar>> planes(ch, QVector<uchar>(n));
    for (int c = 0; c < ch; ++c) {
        for (int y = 0; y < sh; ++y) {
            const uchar *l = small.constScanLine(y);
            for (int x = 0; x < sw; ++x)
                planes[c][y * sw + x] = l[x * ch + c];
        }
        maxFilter(planes[c], sw, sh, 2);
    }
    auto lum = [&](int i) {
        return ch == 1 ? int(planes[0][i])
                       : (299 * planes[0][i] + 587 * planes[1][i] + 114 * planes[2][i]) / 1000;
    };

    // 2. paper colour = mean of the brightest half of the estimate
    QVector<int> lums(n);
    for (int i = 0; i < n; ++i)
        lums[i] = lum(i);
    QVector<int> sorted = lums;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    const int median = sorted[n / 2];
    double paper[3] = {0, 0, 0};
    int cnt = 0;
    for (int i = 0; i < n; ++i)
        if (lums[i] >= median) {
            for (int c = 0; c < ch; ++c)
                paper[c] += planes[c][i];
            ++cnt;
        }
    for (int c = 0; c < ch; ++c)
        paper[c] /= std::max(1, cnt);
    const double paperLum = ch == 1 ? paper[0]
                                    : 0.299 * paper[0] + 0.587 * paper[1] + 0.114 * paper[2];

    // 3. Only paper-like areas count as background: not much darker than the
    //    paper (fold shadows are) and of the same hue. Colour areas, photos
    //    and large dark blocks are content – there the background is filled
    //    in from the surrounding paper instead.
    QVector<char> valid(n);
    for (int i = 0; i < n; ++i) {
        bool ok = lums[i] >= paperLum - 80;
        if (ok && ch == 3) {
            const int rg = planes[0][i] - planes[1][i], bg = planes[2][i] - planes[1][i];
            ok = std::abs(rg - (paper[0] - paper[1])) < 18 && std::abs(bg - (paper[2] - paper[1])) < 18;
        }
        valid[i] = ok;
    }
    for (int iter = 0; iter < 40; ++iter) {
        bool changed = false;
        QVector<char> nextValid = valid;
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                const int i = y * sw + x;
                if (valid[i])
                    continue;
                int sum[3] = {0, 0, 0}, k = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int xx = x + dx, yy = y + dy;
                        if (xx < 0 || yy < 0 || xx >= sw || yy >= sh)
                            continue;
                        const int j = yy * sw + xx;
                        if (!valid[j])
                            continue;
                        for (int c = 0; c < ch; ++c)
                            sum[c] += planes[c][j];
                        ++k;
                    }
                if (k > 0) {
                    for (int c = 0; c < ch; ++c)
                        planes[c][i] = uchar(sum[c] / k);
                    nextValid[i] = 1;
                    changed = true;
                }
            }
        valid.swap(nextValid);
        if (!changed)
            break;
    }
    for (int i = 0; i < n; ++i)
        if (!valid[i])
            for (int c = 0; c < ch; ++c)
                planes[c][i] = uchar(std::lround(paper[c]));

    QImage bg(sw, sh, out.format());
    for (int c = 0; c < ch; ++c) {
        boxBlur(planes[c], sw, sh, 2);
        boxBlur(planes[c], sw, sh, 2);
        for (int y = 0; y < sh; ++y) {
            uchar *l = bg.scanLine(y);
            for (int x = 0; x < sw; ++x)
                l[x * ch + c] = planes[c][y * sw + x];
        }
    }
    const QImage bgFull =
        bg.scaled(out.width(), out.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    // 4. divide by the background; tone curve on neutral pixels only: text
    //    (dark) keeps its value, the light range kToe…kWhite is stretched so
    //    faint remains of creases and bleed-through become white. Coloured
    //    pixels are only divided, so colours are not shifted.
    // strength: the background division fades in over 0…30, the white point
    // moves from 252 (only near-white becomes white) down to 200.
    constexpr int kToe = 150;
    const int kWhite = 252 - strength * 52 / 100;
    const int divide = std::min(256, strength * 256 / 30);   // 256 = full
    uchar curve[256];
    for (int v = 0; v < 256; ++v)
        curve[v] = uchar(v <= kToe ? v
                         : v >= kWhite ? 255
                                       : kToe + (v - kToe) * (255 - kToe) / (kWhite - kToe));
    for (int y = 0; y < out.height(); ++y) {
        uchar *l = out.scanLine(y);
        const uchar *b = bgFull.constScanLine(y);
        for (int x = 0; x < out.width(); ++x) {
            int v[3];
            int lo = 255, hi = 0;
            for (int c = 0; c < ch; ++c) {
                const int o = l[x * ch + c];
                const int full = std::min(255, o * 255 / std::max<int>(b[x * ch + c], 40));
                const int norm = (full * divide + o * (256 - divide)) >> 8;
                // keep dark pixels at their original value: text never gets lighter
                v[c] = o <= kToe ? std::min(o, norm) : norm;
                lo = std::min(lo, v[c]);
                hi = std::max(hi, v[c]);
            }
            const int sat = hi - lo;   // 0 for grey
            const int w = std::clamp((45 - sat) * 256 / 30, 0, 256);   // 256 = neutral
            // Nearly colourless pixels (text, lines, fringes) become true grey:
            // the sheet-feed sensor renders black text with a pink cast.
            if (ch == 3 && w > 0) {
                const int y = (299 * v[0] + 587 * v[1] + 114 * v[2]) / 1000;
                for (int c = 0; c < 3; ++c)
                    v[c] = (y * w + v[c] * (256 - w)) >> 8;
            }
            for (int c = 0; c < ch; ++c)
                l[x * ch + c] = uchar((curve[v[c]] * w + v[c] * (256 - w)) >> 8);
        }
    }
    out.setDotsPerMeterX(img.dotsPerMeterX());
    out.setDotsPerMeterY(img.dotsPerMeterY());
    return out;
}

QImage adjustContrast(const QImage &img, int amount)
{
    if (amount == 0 || img.isNull() || img.format() == QImage::Format_Mono
        || img.format() == QImage::Format_MonoLSB)
        return img;
    const bool grey = img.format() == QImage::Format_Grayscale8;
    QImage out = img.convertToFormat(grey ? QImage::Format_Grayscale8 : QImage::Format_RGB888);

    // mean brightness from a small copy
    const QImage small = out.convertToFormat(QImage::Format_Grayscale8)
                             .scaledToWidth(std::min(out.width(), 400), Qt::FastTransformation);
    qint64 sum = 0;
    for (int y = 0; y < small.height(); ++y) {
        const uchar *l = small.constScanLine(y);
        for (int x = 0; x < small.width(); ++x)
            sum += l[x];
    }
    const double pivot = double(sum) / (qint64(small.width()) * small.height());
    const double f = std::pow(2.0, amount / 50.0);   // -50 → ½, +50 → 2

    uchar lut[256];
    for (int v = 0; v < 256; ++v)
        lut[v] = uchar(std::clamp(std::lround(pivot + (v - pivot) * f), 0L, 255L));

    const int bytes = out.width() * (grey ? 1 : 3);
    for (int y = 0; y < out.height(); ++y) {
        uchar *l = out.scanLine(y);
        for (int x = 0; x < bytes; ++x)
            l[x] = lut[l[x]];
    }
    return out;
}

QImage deskew(const QImage &img)
{
    const double angle = detectSkew(img);
    if (std::abs(angle) < kMinAngle)
        return img;

    const bool mono = img.format() == QImage::Format_Mono
                      || img.format() == QImage::Format_MonoLSB;
    const QImage::Format work = img.format() == QImage::Format_Grayscale8 || mono
                                    ? QImage::Format_Grayscale8
                                    : QImage::Format_RGB888;
    const QImage src = img.convertToFormat(work);
    QImage out(src.size(), work);
    out.fill(Qt::white);
    {
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.translate(out.width() / 2.0, out.height() / 2.0);
        p.rotate(-angle);
        p.translate(-src.width() / 2.0, -src.height() / 2.0);
        p.drawImage(0, 0, src);
    }
    if (mono) {
        QImage m = out.convertToFormat(QImage::Format_Mono, Qt::ThresholdDither);
        // keep the "bit 1 = black" convention used elsewhere
        if (m.colorCount() == 2 && qGray(m.color(1)) > qGray(m.color(0))) {
            m.invertPixels();
            m.setColorTable({m.color(1), m.color(0)});
        }
        out = m;
    }
    out.setDotsPerMeterX(img.dotsPerMeterX());
    out.setDotsPerMeterY(img.dotsPerMeterY());
    return out;
}

bool isBlank(const QImage &img)
{
    const QImage g = img.convertToFormat(QImage::Format_Grayscale8);
    const int mx = g.width() / 16, my = g.height() / 16;
    qint64 dark = 0, total = 0;
    for (int y = my; y < g.height() - my; y += 2) {
        const uchar *line = g.constScanLine(y);
        for (int x = mx; x < g.width() - mx; x += 2) {
            ++total;
            if (line[x] < 200)
                ++dark;
        }
    }
    return total > 0 && double(dark) / double(total) < 0.0002;
}

QImage toGray(const QImage &img)
{
    if (img.format() == QImage::Format_Grayscale8)
        return img;
    if (img.format() == QImage::Format_Mono || img.format() == QImage::Format_MonoLSB)
        return img;
    const QImage rgb = img.convertToFormat(QImage::Format_RGB888);
    QImage g(rgb.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < rgb.height(); ++y) {
        const uchar *s = rgb.constScanLine(y);
        uchar *d = g.scanLine(y);
        for (int x = 0; x < rgb.width(); ++x)
            d[x] = uchar((299 * s[3 * x] + 587 * s[3 * x + 1] + 114 * s[3 * x + 2]) / 1000);
    }
    g.setDotsPerMeterX(img.dotsPerMeterX());
    g.setDotsPerMeterY(img.dotsPerMeterY());
    return g;
}

QImage toLineart(const QImage &img)
{
    if (img.format() == QImage::Format_Mono || img.format() == QImage::Format_MonoLSB)
        return img;
    const QImage g = toGray(img);
    // Otsu threshold from the histogram
    QVector<qint64> hist(256, 0);
    for (int y = 0; y < g.height(); y += 2) {
        const uchar *l = g.constScanLine(y);
        for (int x = 0; x < g.width(); x += 2)
            ++hist[l[x]];
    }
    qint64 total = 0;
    double sumAll = 0;
    for (int v = 0; v < 256; ++v) {
        total += hist[v];
        sumAll += double(v) * hist[v];
    }
    double sumB = 0, best = -1;
    qint64 wB = 0;
    int thr = 160;
    for (int t = 0; t < 256; ++t) {
        wB += hist[t];
        if (wB == 0)
            continue;
        const qint64 wF = total - wB;
        if (wF == 0)
            break;
        sumB += double(t) * hist[t];
        const double mB = sumB / wB, mF = (sumAll - sumB) / wF;
        const double between = double(wB) * double(wF) * (mB - mF) * (mB - mF);
        if (between > best) {
            best = between;
            thr = t;
        }
    }
    thr = std::clamp(thr, 90, 215);   // guard against near-empty pages

    QImage m(g.size(), QImage::Format_Mono);
    m.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});   // bit 1 = black
    m.fill(0);
    for (int y = 0; y < g.height(); ++y) {
        const uchar *s = g.constScanLine(y);
        uchar *d = m.scanLine(y);
        for (int x = 0; x < g.width(); ++x)
            if (s[x] <= thr)
                d[x >> 3] |= uchar(0x80 >> (x & 7));
    }
    m.setDotsPerMeterX(img.dotsPerMeterX());
    m.setDotsPerMeterY(img.dotsPerMeterY());
    return m;
}

QImage process(const QImage &raw, const PageSettings &ps)
{
    QImage out = ps.flatten > 0 ? flattenBackground(raw, ps.flatten) : raw;
    out = adjustContrast(out, ps.contrast);
    if (ps.mode != ColorMode::Color)
        out = toGray(out);
    if (ps.deskew)
        out = deskew(out);   // on grey rather than on black-and-white: cleaner edges
    if (ps.mode == ColorMode::Lineart)
        out = toLineart(out);
    return out;
}

} // namespace ImageOps
