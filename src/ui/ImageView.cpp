#include "ui/ImageView.h"

#include "annotation/AnnotationModel.h"
#include "annotation/ClassColors.h"

#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QImage>
#include <QKeyEvent>
#include <QLineF>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPolygonF>
#include <QResizeEvent>
#include <QTimer>

#include <algorithm>

namespace {
constexpr double kVertexRadiusPx = 4.0;   // on-screen vertex marker radius
constexpr double kCloseEnoughPx  = 12.0;  // click within this of vertex 0 closes a polygon
constexpr double kGrabRadiusPx   = 9.0;   // hit radius to grab a committed vertex
constexpr double kEdgeGrabPx     = 6.0;   // hit distance to grab a committed edge
constexpr double kMoveCancelPx   = 8.0;   // movement that cancels a pending long-press
constexpr int    kLongPressMs    = 350;   // hold time to start a vertex drag

const QColor kDrawingColor(0x33, 0xcc, 0xff);  // cyan: shape being drawn
const QColor kPendingColor(0xff, 0xcc, 0x33);  // amber: shape awaiting a class
const QColor kFallbackColor(0x3c, 0xb3, 0x71); // green: when no class palette is set

// Shortest distance from point p to segment [a, b].
double pointSegmentDistance(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a;
    const QPointF ap = p - a;
    const double denom = ab.x() * ab.x() + ab.y() * ab.y();
    if (denom <= 0.0) {
        return QLineF(p, a).length();
    }
    double t = (ap.x() * ab.x() + ap.y() * ab.y()) / denom;
    t = std::clamp(t, 0.0, 1.0);
    const QPointF proj(a.x() + t * ab.x(), a.y() + t * ab.y());
    return QLineF(p, proj).length();
}

QPolygonF shapeOutline(const Shape &s)
{
    if (s.type == Shape::Type::Rectangle && s.points.size() == 2) {
        return QPolygonF(QRectF(s.points[0], s.points[1]).normalized());
    }
    return QPolygonF(s.points);
}
} // namespace

ImageView::ImageView(QWidget *parent)
    : QGraphicsView(parent)
    , m_scene(new QGraphicsScene(this))
    , m_pixmapItem(m_scene->addPixmap(QPixmap()))
    , m_longPress(new QTimer(this))
{
    setScene(m_scene);
    setDragMode(QGraphicsView::NoDrag);
    setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing);
    setAlignment(Qt::AlignCenter);
    setBackgroundBrush(QColor(0x2b, 0x2b, 0x2b));
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    m_longPress->setSingleShot(true);
    m_longPress->setInterval(kLongPressMs);
    connect(m_longPress, &QTimer::timeout, this, &ImageView::onLongPress);
}

bool ImageView::hasImage() const
{
    return !m_pixmapItem->pixmap().isNull();
}

void ImageView::setModel(AnnotationModel *model)
{
    m_model = model;
    if (m_model) {
        connect(m_model, &AnnotationModel::changed,
                viewport(), QOverload<>::of(&QWidget::update));
    }
}

void ImageView::setClassColors(const ClassColors *colors)
{
    m_classColors = colors;
    viewport()->update();
}

void ImageView::setMode(Mode mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        resetInteraction();
        viewport()->update();
    }
}

void ImageView::setImage(const QImage &image)
{
    resetInteraction();
    if (image.isNull()) {
        clear();
        return;
    }
    m_pixmapItem->setPixmap(QPixmap::fromImage(image));
    m_scene->setSceneRect(m_pixmapItem->boundingRect());
    fitImage();
}

void ImageView::clear()
{
    resetInteraction();
    m_pixmapItem->setPixmap(QPixmap());
    m_scene->setSceneRect(QRectF());
    resetTransform();
    viewport()->update();
}

void ImageView::resetInteraction()
{
    m_drawing = false;
    m_pending = false;
    m_current.clear();
    m_pendingShape = ::Shape();
    cancelLongPress();
    m_dragging = false;
    m_dragShape = m_dragVertex = -1;
    m_hoverShape = m_hoverVertex = -1;
    unsetCursor();
}

void ImageView::acceptPendingShape()
{
    // Convenience: if the user is mid-polygon with enough points but hasn't
    // clicked back on the first vertex, Accept finalizes it for them.
    if (!m_pending && m_drawing && m_mode == Mode::Polygon && m_current.size() >= 3) {
        m_pendingShape = ::Shape();
        m_pendingShape.type   = ::Shape::Type::Polygon;
        m_pendingShape.points = m_current;
        m_pending  = true;
        m_drawing  = false;
        m_current.clear();
    }

    if (m_pending && m_pendingShape.isComplete()) {
        const ::Shape shape = m_pendingShape;
        resetInteraction();
        viewport()->update();
        emit shapeDrawn(shape);   // MainWindow assigns class + exports
    }
}

void ImageView::cancelLongPress()
{
    m_longPress->stop();
    m_pressActive = false;
    m_pressShape = m_pressVertex = m_edgeInsert = -1;
}

void ImageView::fitImage()
{
    if (hasImage()) {
        fitInView(m_pixmapItem, Qt::KeepAspectRatio);
    }
}

double ImageView::sceneScale() const
{
    const double s = transform().m11();
    return s != 0.0 ? s : 1.0;
}

QPointF ImageView::clampToImage(const QPointF &p) const
{
    const QRectF r = m_scene->sceneRect();
    return QPointF(std::clamp(p.x(), 0.0, r.width()),
                   std::clamp(p.y(), 0.0, r.height()));
}

// --- hit testing -----------------------------------------------------------

ImageView::VertexHit ImageView::hitVertex(const QPointF &p) const
{
    if (!m_model) {
        return {};
    }
    const double r = kGrabRadiusPx / sceneScale();
    const QVector<::Shape> &shapes = m_model->shapes();
    for (int si = shapes.size() - 1; si >= 0; --si) {  // topmost first
        const QVector<QPointF> &pts = shapes[si].points;
        for (int vi = 0; vi < pts.size(); ++vi) {
            if (QLineF(p, pts[vi]).length() <= r) {
                return { si, vi };
            }
        }
    }
    return {};
}

ImageView::EdgeHit ImageView::hitEdge(const QPointF &p) const
{
    if (!m_model) {
        return {};
    }
    const double tol = kEdgeGrabPx / sceneScale();
    const QVector<::Shape> &shapes = m_model->shapes();
    for (int si = shapes.size() - 1; si >= 0; --si) {
        const ::Shape &s = shapes[si];
        if (s.type != ::Shape::Type::Polygon || s.points.size() < 3) {
            continue;  // vertex insertion only makes sense for polygons
        }
        const int n = s.points.size();
        for (int i = 0; i < n; ++i) {
            const QPointF &a = s.points[i];
            const QPointF &b = s.points[(i + 1) % n];
            if (pointSegmentDistance(p, a, b) <= tol) {
                return { si, i + 1 };  // insert after vertex i
            }
        }
    }
    return {};
}

int ImageView::hitShape(const QPointF &p) const
{
    if (!m_model) {
        return -1;
    }
    const double tol = kEdgeGrabPx / sceneScale();
    const QVector<::Shape> &shapes = m_model->shapes();
    for (int si = shapes.size() - 1; si >= 0; --si) {
        const QPolygonF outline = shapeOutline(shapes[si]);
        if (outline.size() < 2) {
            continue;
        }
        if (outline.containsPoint(p, Qt::OddEvenFill)) {
            return si;
        }
        for (int i = 0; i < outline.size(); ++i) {
            const QPointF &a = outline[i];
            const QPointF &b = outline[(i + 1) % outline.size()];
            if (pointSegmentDistance(p, a, b) <= tol) {
                return si;
            }
        }
    }
    return -1;
}

// --- interaction -----------------------------------------------------------

void ImageView::mousePressEvent(QMouseEvent *event)
{
    if (!hasImage()) {
        return;
    }

    const QPointF pos = clampToImage(mapToScene(event->pos()));

    if (m_pending) {
        return;  // pending shape: only Enter/Esc proceed
    }

    if (event->button() == Qt::RightButton) {
        if (m_drawing && !m_current.isEmpty()) {
            m_current.removeLast();          // undo last placed vertex
            if (m_current.isEmpty()) {
                m_drawing = false;
            }
            viewport()->update();
        } else if (!m_drawing) {
            showContextMenu(event->globalPosition().toPoint(), pos);
        }
        return;
    }

    if (event->button() != Qt::LeftButton) {
        return;
    }

    // When idle, a press on a committed vertex/edge may begin an edit (after a
    // long press). Drawing-on-empty-space is unaffected and stays immediate.
    if (!m_drawing) {
        const VertexHit vh = hitVertex(pos);
        if (vh.shape >= 0) {
            m_pressActive = true; m_pressScene = pos;
            m_pressShape = vh.shape; m_pressVertex = vh.vertex; m_edgeInsert = -1;
            m_longPress->start();
            return;
        }
        const EdgeHit eh = hitEdge(pos);
        if (eh.shape >= 0) {
            m_pressActive = true; m_pressScene = pos;
            m_pressShape = eh.shape; m_pressVertex = -1; m_edgeInsert = eh.insertIndex;
            m_longPress->start();
            return;
        }
    }

    // Otherwise: draw a new shape.
    if (m_mode == Mode::Rectangle) {
        if (!m_drawing) {
            m_drawing = true;
            m_current = { pos };
        } else {
            m_drawing = false;
            m_pendingShape = ::Shape();
            m_pendingShape.type   = ::Shape::Type::Rectangle;
            m_pendingShape.points = { m_current.first(), pos };
            m_pending = m_pendingShape.isComplete();
            m_current.clear();
        }
    } else {  // Polygon
        if (m_drawing && m_current.size() >= 3) {
            const double closeScene = kCloseEnoughPx / sceneScale();
            if (QLineF(pos, m_current.first()).length() <= closeScene) {
                m_drawing = false;
                m_pendingShape = ::Shape();
                m_pendingShape.type   = ::Shape::Type::Polygon;
                m_pendingShape.points = m_current;
                m_pending = m_pendingShape.isComplete();
                m_current.clear();
                viewport()->update();
                return;
            }
        }
        m_drawing = true;
        m_current.append(pos);
    }

    viewport()->update();
}

void ImageView::onLongPress()
{
    if (!m_pressActive || !m_model || m_pressShape < 0) {
        return;
    }
    m_dragWorking = m_model->shapes()[m_pressShape];

    if (m_pressVertex >= 0) {
        m_dragShape  = m_pressShape;
        m_dragVertex = m_pressVertex;
    } else if (m_edgeInsert >= 0) {
        // Insert a new vertex at the press position, then drag it.
        m_dragWorking.points.insert(m_edgeInsert, m_pressScene);
        m_dragShape  = m_pressShape;
        m_dragVertex = m_edgeInsert;
    }

    m_dragging = true;
    m_pressActive = false;
    setCursor(Qt::ClosedHandCursor);
    viewport()->update();
}

void ImageView::mouseMoveEvent(QMouseEvent *event)
{
    if (!hasImage()) {
        return;
    }
    m_cursor = clampToImage(mapToScene(event->pos()));

    if (m_dragging) {
        m_dragWorking.points[m_dragVertex] = m_cursor;
        viewport()->update();
        return;
    }

    if (m_pressActive) {
        // Moved too far before the hold completed -> cancel the edit gesture.
        if (QLineF(m_cursor, m_pressScene).length() * sceneScale() > kMoveCancelPx) {
            cancelLongPress();
        }
        return;
    }

    if (m_drawing) {
        viewport()->update();  // live preview to the cursor
        return;
    }

    // Idle: highlight a committed vertex under the cursor.
    const VertexHit vh = hitVertex(m_cursor);
    if (vh.shape != m_hoverShape || vh.vertex != m_hoverVertex) {
        m_hoverShape = vh.shape;
        m_hoverVertex = vh.vertex;
        setCursor(vh.shape >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
        viewport()->update();
    }
}

void ImageView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_model->updateShape(m_dragShape, m_dragWorking);  // commit the moved vertex
        m_dragging = false;
        m_dragShape = m_dragVertex = -1;
        setCursor(Qt::OpenHandCursor);
        emit annotationsChanged();
        viewport()->update();
        return;
    }
    if (m_pressActive && event->button() == Qt::LeftButton) {
        cancelLongPress();  // a quick tap on a feature: no drag
    }
}

void ImageView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        acceptPendingShape();
        return;
    case Qt::Key_Escape:
        if (m_drawing || m_pending || m_dragging || m_pressActive) {
            resetInteraction();
            viewport()->update();
        }
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (!m_drawing && !m_pending && !m_dragging) {
            emit removeLastRequested();
        }
        return;
    default:
        QGraphicsView::keyPressEvent(event);
    }
}

void ImageView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    fitImage();
}

void ImageView::showContextMenu(const QPoint &globalPos, const QPointF &scenePos)
{
    if (!m_model) {
        return;
    }
    const VertexHit vh = hitVertex(scenePos);
    const int shapeIdx = vh.shape >= 0 ? vh.shape : hitShape(scenePos);
    if (shapeIdx < 0) {
        return;
    }

    const ::Shape &s = m_model->shapes()[shapeIdx];
    QMenu menu(this);

    QAction *delVertex = nullptr;
    if (vh.shape >= 0 && s.type == ::Shape::Type::Polygon && s.points.size() > 3) {
        delVertex = menu.addAction(tr("Delete vertex"));
    }
    QAction *delShape = menu.addAction(tr("Delete shape"));

    QAction *chosen = menu.exec(globalPos);
    if (!chosen) {
        return;
    }
    if (chosen == delVertex) {
        ::Shape edited = s;
        edited.points.removeAt(vh.vertex);
        m_model->updateShape(vh.shape, edited);
        emit annotationsChanged();
    } else if (chosen == delShape) {
        m_model->removeAt(shapeIdx);
        emit annotationsChanged();
    }
}

// --- rendering -------------------------------------------------------------

void ImageView::paintEvent(QPaintEvent *event)
{
    QGraphicsView::paintEvent(event);
    if (hasImage()) {
        return;
    }
    QPainter painter(viewport());
    painter.setPen(QColor(0xb0, 0xb0, 0xb0));
    painter.drawText(viewport()->rect(), Qt::AlignCenter,
                     tr("No image loaded.\nUse File -> Open Image Folder..."));
}

void ImageView::drawShape(QPainter *painter, const ::Shape &shape, double radius) const
{
    if (shape.points.isEmpty()) {
        return;
    }
    if (shape.type == ::Shape::Type::Rectangle && shape.points.size() == 2) {
        painter->drawRect(QRectF(shape.points[0], shape.points[1]).normalized());
    } else {
        for (int i = 0; i + 1 < shape.points.size(); ++i) {
            painter->drawLine(shape.points[i], shape.points[i + 1]);
        }
        if (shape.type == ::Shape::Type::Polygon && shape.points.size() >= 3) {
            painter->drawLine(shape.points.last(), shape.points.first());
        }
    }
    const QBrush savedBrush = painter->brush();
    painter->setBrush(painter->pen().color());
    for (const QPointF &p : shape.points) {
        painter->drawEllipse(p, radius, radius);
    }
    painter->setBrush(savedBrush);
}

void ImageView::drawForeground(QPainter *painter, const QRectF &)
{
    if (!hasImage()) {
        return;
    }
    const double scale  = sceneScale();
    const double radius = kVertexRadiusPx / scale;

    QPen pen;
    pen.setCosmetic(true);
    pen.setWidth(2);
    painter->setBrush(Qt::NoBrush);

    // Committed shapes, each in its class colour (drag uses the live copy).
    if (m_model) {
        const QVector<::Shape> &shapes = m_model->shapes();
        for (int i = 0; i < shapes.size(); ++i) {
            const ::Shape &s = (m_dragging && i == m_dragShape) ? m_dragWorking : shapes[i];
            const QColor color = m_classColors ? m_classColors->colorFor(s.classId)
                                               : kFallbackColor;
            pen.setColor(color);
            painter->setPen(pen);
            drawShape(painter, s, radius);
        }
    }

    // Hover highlight: a hollow ring around the vertex that would be grabbed.
    if (!m_dragging && m_hoverShape >= 0 && m_model
        && m_hoverShape < m_model->shapes().size()) {
        const QVector<QPointF> &pts = m_model->shapes()[m_hoverShape].points;
        if (m_hoverVertex >= 0 && m_hoverVertex < pts.size()) {
            pen.setColor(Qt::white);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(pts[m_hoverVertex], radius * 1.8, radius * 1.8);
        }
    }

    // Shape awaiting a class. Polygons get a translucent fill so a SAM preview
    // reads like a mask, not just an outline.
    if (m_pending) {
        if (m_pendingShape.type == ::Shape::Type::Polygon && m_pendingShape.points.size() >= 3) {
            QColor fill = kPendingColor;
            fill.setAlpha(70);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawPolygon(QPolygonF(m_pendingShape.points));
            painter->setBrush(Qt::NoBrush);
        }
        pen.setColor(kPendingColor);
        painter->setPen(pen);
        drawShape(painter, m_pendingShape, radius);
    }

    // Shape being drawn, with a live edge to the cursor.
    if (m_drawing && !m_current.isEmpty()) {
        pen.setColor(kDrawingColor);
        painter->setPen(pen);
        if (m_mode == Mode::Rectangle) {
            painter->drawRect(QRectF(m_current.first(), m_cursor).normalized());
        } else {
            for (int i = 0; i + 1 < m_current.size(); ++i) {
                painter->drawLine(m_current[i], m_current[i + 1]);
            }
            painter->drawLine(m_current.last(), m_cursor);
        }
        painter->setBrush(kDrawingColor);
        for (const QPointF &p : m_current) {
            painter->drawEllipse(p, radius, radius);
        }
        painter->setBrush(Qt::NoBrush);
    }
}
