#pragma once

#include <QImage>

// Platform-independent page processing. Scanners deliver raw pages; the app
// keeps them and derives the visible page from raw + PageSettings, so the
// settings can still be changed after scanning.
namespace ImageOps {

enum class ColorMode { Color, Gray, Lineart };

struct PageSettings {
    int flatten = 50;      // background evening 0 (off) … 100
    bool deskew = true;    // straighten
    int contrast = 0;      // -50 … +50
    // output colour mode; can only reduce what the scan contains
    // (a grey scan stays grey when Color is chosen)
    ColorMode mode = ColorMode::Color;
    bool operator==(const PageSettings &o) const
    {
        return flatten == o.flatten && deskew == o.deskew && contrast == o.contrast
               && mode == o.mode;
    }
};

// True when the page carries (almost) nothing but paper. The sheet-feed
// scanner renders text in greys rather than deep black, so "ink" is anything
// below 200; bleed-through from the other side stays lighter than that.
bool isBlank(const QImage &img);

// Grey copy using standard luma weights on the stored (sRGB) values.
QImage toGray(const QImage &img);
// Black and white with a threshold chosen from the page itself (Otsu), so
// light-grey text of the sheet-feed scanner is kept.
QImage toLineart(const QImage &img);

// Applies all settings in the right order
// (background, contrast, colour reduction, deskew, threshold).
QImage process(const QImage &raw, const PageSettings &ps);

// Skew angle of the page content in degrees (positive = rotated clockwise),
// found from the text lines. 0 when nothing reliable was found.
double detectSkew(const QImage &img);

// Contrast in the range -50 … +50 (0 = unchanged). The pivot is the page's
// mean brightness, so on documents the paper stays white and text gets
// darker, while photos behave like a normal contrast control.
QImage adjustContrast(const QImage &img, int amount);

// Evens out the paper background: shadows of folds and creases, uneven
// lighting, yellowed paper and colour casts become plain white while text and
// lines stay. Meant for documents – it flattens the tonal range of photos.
// strength 1 … 100: low values only even out the background, higher values
// also turn light grey remains (creases, bleed-through) white.
QImage flattenBackground(const QImage &img, int strength = 50);

// Straightens the page; corners that become free are filled white.
// Returns the input unchanged when the skew is negligible.
QImage deskew(const QImage &img);

} // namespace ImageOps
