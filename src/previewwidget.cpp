#include "previewwidget.h"
#include "theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kMaxZoom = 16.0;
constexpr double kMargin = 12.0;
} // namespace

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(300, 300);
    setMouseTracking(true);
}

void PreviewWidget::setImage(const QImage &img, bool selectable, double mmPerPixel)
{
    m_img = img;
    m_totalHeight = img.height();
    m_selectable = selectable && mmPerPixel > 0;
    m_mmPerPx = mmPerPixel;
    m_sel = {};
    m_dragging = false;
    resetView();
}

void PreviewWidget::setLiveImage(const QImage &img, int totalHeight)
{
    // keep zoom and pan: the user may be watching a detail while it scans
    const bool first = m_img.isNull() || m_selectable;
    m_img = img;
    m_totalHeight = std::max(img.height(), totalHeight);
    m_selectable = false;
    m_sel = {};
    if (first)
        resetView();
    else {
        clampOffset();
        updateCursor();
        update();
    }
}

void PreviewWidget::replaceImage(const QImage &img)
{
    if (m_img.isNull() || m_selectable || img.size() != m_img.size()) {
        setImage(img);
        return;
    }
    m_img = img;
    m_totalHeight = img.height();
    update();
}

void PreviewWidget::setPlaceholder(const QString &text)
{
    m_placeholder = text;
    update();
}

void PreviewWidget::clear()
{
    setImage({});
}

QRectF PreviewWidget::selectionMm() const
{
    if (!m_selectable || m_sel.isEmpty())
        return {};
    return QRectF(m_sel.topLeft() * m_mmPerPx, m_sel.size() * m_mmPerPx);
}

void PreviewWidget::clearSelection()
{
    m_sel = {};
    update();
    emit selectionChanged({});
}

// --- geometry -------------------------------------------------------------------

QRectF PreviewWidget::viewArea() const
{
    return QRectF(rect()).adjusted(kMargin, kMargin, -kMargin, -kMargin);
}

QRectF PreviewWidget::fitRect() const
{
    if (m_img.isNull())
        return {};
    const QSizeF full(m_img.width(), m_totalHeight);
    const QRectF area = viewArea();
    const QSizeF s = full.scaled(area.size(), Qt::KeepAspectRatio);
    return QRectF(area.center().x() - s.width() / 2, area.center().y() - s.height() / 2,
                  s.width(), s.height());
}

QRectF PreviewWidget::targetRect() const
{
    const QRectF f = fitRect();
    if (f.isEmpty())
        return f;
    const QSizeF s = f.size() * m_zoom;
    const QPointF c = f.center() + m_offset;
    return QRectF(c.x() - s.width() / 2, c.y() - s.height() / 2, s.width(), s.height());
}

QPointF PreviewWidget::toImage(const QPointF &w) const
{
    const QRectF t = targetRect();
    if (t.isEmpty())
        return {};
    const double scale = m_img.width() / t.width();
    QPointF p = (w - t.topLeft()) * scale;
    p.setX(std::clamp(p.x(), 0.0, double(m_img.width())));
    p.setY(std::clamp(p.y(), 0.0, double(m_img.height())));
    return p;
}

// Keeps the page inside the view: centred while it is smaller than the view,
// otherwise no empty gap at the edges.
void PreviewWidget::clampOffset()
{
    const QRectF area = viewArea();
    const QRectF f = fitRect();
    if (f.isEmpty()) {
        m_offset = {};
        return;
    }
    const QSizeF s = f.size() * m_zoom;
    auto clampAxis = [](double offset, double size, double viewSize) {
        const double slack = (size - viewSize) / 2;
        return slack <= 0 ? 0.0 : std::clamp(offset, -slack, slack);
    };
    // fitRect is centred in the area, so the offset is relative to that centre
    m_offset.setX(clampAxis(m_offset.x(), s.width(), area.width()));
    m_offset.setY(clampAxis(m_offset.y(), s.height(), area.height()));
}

void PreviewWidget::zoomAt(const QPointF &pos, double factor)
{
    const QRectF before = targetRect();
    if (before.isEmpty())
        return;
    const double newZoom = std::clamp(m_zoom * factor, 1.0, kMaxZoom);
    if (qFuzzyCompare(newZoom, m_zoom))
        return;
    // keep the image point under the mouse where it is
    const QPointF rel((pos.x() - before.left()) / before.width(),
                      (pos.y() - before.top()) / before.height());
    m_zoom = newZoom;
    const QRectF after = targetRect();
    const QPointF moved(after.left() + rel.x() * after.width(),
                        after.top() + rel.y() * after.height());
    m_offset += pos - moved;
    clampOffset();
    updateCursor();
    update();
}

void PreviewWidget::resetView()
{
    m_zoom = 1.0;
    m_offset = {};
    m_panning = false;
    updateCursor();
    update();
}

void PreviewWidget::updateCursor()
{
    if (m_panning)
        setCursor(Qt::ClosedHandCursor);
    else if (m_selectable)
        setCursor(Qt::CrossCursor);
    else if (m_zoom > 1.0)
        setCursor(Qt::OpenHandCursor);
    else
        setCursor(Qt::ArrowCursor);
}

// --- painting ---------------------------------------------------------------------

void PreviewWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // card background like the other panels
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QColor(255, 255, 255, 20));
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawRoundedRect(card, 14, 14);

    if (m_img.isNull()) {
        p.setPen(Theme::TextDim);
        p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                   m_placeholder);
        return;
    }

    QPainterPath clip;
    clip.addRoundedRect(card.adjusted(1, 1, -1, -1), 13, 13);
    p.setClipPath(clip);

    const QRectF t = targetRect();
    const double scale = t.width() / m_img.width();
    // page area not yet scanned (live mode)
    if (m_totalHeight > m_img.height()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 18));
        p.drawRect(t);
    }
    const QRectF drawn(t.topLeft(), QSizeF(t.width(), m_img.height() * scale));
    // only draw the visible part: keeps zooming into 600 dpi pages fluid
    const QRectF visible = drawn.intersected(QRectF(rect()));
    if (!visible.isEmpty()) {
        const QRectF src((visible.left() - drawn.left()) / scale,
                         (visible.top() - drawn.top()) / scale, visible.width() / scale,
                         visible.height() / scale);
        p.drawImage(visible, m_img, src);
    }
    if (m_totalHeight > m_img.height()) {   // scan line
        p.setPen(QPen(Theme::Orange, 2));
        p.drawLine(drawn.bottomLeft(), drawn.bottomRight());
    }

    if (m_selectable && !m_sel.isEmpty()) {
        const QRectF s(t.topLeft() + m_sel.topLeft() * scale, m_sel.size() * scale);
        QPainterPath outside;
        outside.addRect(drawn);
        QPainterPath inside;
        inside.addRect(s);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 120));
        p.drawPath(outside.subtracted(inside));
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(Theme::Orange, 2));
        p.drawRect(s);
        const QRectF mm = selectionMm();
        const QString label = QStringLiteral("%1 × %2 mm")
                                  .arg(qRound(mm.width())).arg(qRound(mm.height()));
        QRectF lr = p.fontMetrics().boundingRect(label).adjusted(-6, -3, 6, 3);
        lr.moveTopLeft(s.topLeft() + QPointF(4, 4));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x0b, 0x0e, 0x13, 220));
        p.drawRoundedRect(lr, 4, 4);
        p.setPen(Theme::Orange);
        p.drawText(lr, Qt::AlignCenter, label);
    }

    if (m_zoom > 1.0) {   // zoom badge
        const QString label = QStringLiteral("%1 %").arg(qRound(m_zoom * 100));
        QRectF lr = p.fontMetrics().boundingRect(label).adjusted(-8, -4, 8, 4);
        lr.moveTopRight(QPointF(width() - kMargin - 4, kMargin + 4));
        p.setPen(QPen(QColor(0xff, 0x8a, 0x00, 115), 1));
        p.setBrush(QColor(0x0b, 0x0e, 0x13, 220));
        p.drawRoundedRect(lr, 6, 6);
        p.setPen(Theme::Orange);
        p.drawText(lr, Qt::AlignCenter, label);
    }
}

// --- interaction ---------------------------------------------------------------------

void PreviewWidget::mousePressEvent(QMouseEvent *e)
{
    if (m_img.isNull())
        return;
    const bool panButton = e->button() == Qt::MiddleButton
                           || (e->button() == Qt::LeftButton && !m_selectable);
    if (panButton && m_zoom > 1.0) {
        m_panning = true;
        m_panStart = e->position();
        m_panOffsetStart = m_offset;
        updateCursor();
        return;
    }
    if (!m_selectable)
        return;
    if (e->button() == Qt::RightButton) {
        clearSelection();
        return;
    }
    if (e->button() == Qt::LeftButton) {
        m_dragStart = toImage(e->position());
        m_sel = QRectF(m_dragStart, QSizeF());
        m_dragging = true;
        update();
    }
}

void PreviewWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_panning) {
        m_offset = m_panOffsetStart + (e->position() - m_panStart);
        clampOffset();
        update();
        return;
    }
    if (!m_dragging)
        return;
    m_sel = QRectF(m_dragStart, toImage(e->position())).normalized();
    update();
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_panning) {
        m_panning = false;
        updateCursor();
        return;
    }
    if (!m_dragging || e->button() != Qt::LeftButton)
        return;
    m_dragging = false;
    // a click without dragging removes the selection
    if (m_sel.width() < 4 || m_sel.height() < 4)
        m_sel = {};
    update();
    emit selectionChanged(selectionMm());
}

void PreviewWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !m_img.isNull())
        resetView();
}

void PreviewWidget::wheelEvent(QWheelEvent *e)
{
    if (m_img.isNull()) {
        e->ignore();
        return;
    }
    const QPoint deg = e->angleDelta();
    if (e->modifiers() & Qt::ControlModifier) {
        const double steps = deg.y() / 120.0;
        zoomAt(e->position(), std::pow(1.25, steps));
        e->accept();
        return;
    }
    if (m_zoom <= 1.0) {
        e->ignore();
        return;
    }
    // scroll the zoomed page; Shift (or a horizontal wheel) scrolls sideways
    QPointF delta = e->pixelDelta().isNull() ? QPointF(deg) / 2.0 : QPointF(e->pixelDelta());
    if (e->modifiers() & Qt::ShiftModifier)
        delta = QPointF(delta.y(), delta.x());
    m_offset += delta;
    clampOffset();
    update();
    e->accept();
}

void PreviewWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    clampOffset();
}
