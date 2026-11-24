#pragma once

#include <QImage>
#include <QRectF>
#include <QWidget>

// Large page view. Shows either a scanned page, a page that is still being
// scanned (live), or a flatbed quick-scan in which the user can drag a
// rectangle to choose the area for the real scan.
//
// Ctrl+wheel zooms around the mouse pointer, dragging pans (left button, or
// middle button while selecting an area), the wheel scrolls a zoomed page and
// a double-click fits the page back into the view.
class PreviewWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget *parent = nullptr);

    // Shows an image. With selectable=true the user can drag a scan area;
    // mmPerPixel converts image pixels to millimetres on the scanner glass.
    void setImage(const QImage &img, bool selectable = false, double mmPerPixel = 0);
    // Live update during a scan: img grows downwards; totalHeight (pixels)
    // reserves space for the full page when the backend knows it.
    void setLiveImage(const QImage &img, int totalHeight);
    // Swaps in a re-processed version of the shown page, keeping zoom and pan.
    void replaceImage(const QImage &img);
    void setPlaceholder(const QString &text);
    void clear();

    QRectF selectionMm() const;   // empty when nothing is selected
    void clearSelection();

signals:
    void selectionChanged(const QRectF &mm);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;

private:
    QRectF viewArea() const;                   // inner area of the card
    QRectF fitRect() const;                    // page fitted into viewArea
    QRectF targetRect() const;                 // page incl. zoom and pan
    QPointF toImage(const QPointF &w) const;   // widget -> image pixels
    void zoomAt(const QPointF &pos, double factor);
    void clampOffset();
    void resetView();
    void updateCursor();

    QImage m_img;
    int m_totalHeight = 0;
    bool m_selectable = false;
    double m_mmPerPx = 0;
    QRectF m_sel;          // in image pixels
    QPointF m_dragStart;
    bool m_dragging = false;
    QString m_placeholder;

    double m_zoom = 1.0;   // 1 = page fits the view
    QPointF m_offset;      // pan, in widget pixels
    bool m_panning = false;
    QPointF m_panStart, m_panOffsetStart;
};
