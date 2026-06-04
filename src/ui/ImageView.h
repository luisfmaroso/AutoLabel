#pragma once

#include "annotation/Shape.h"

#include <QGraphicsView>
#include <QPointF>
#include <QVector>

// Note: this class derives from QFrame (via QGraphicsView), which has its own
// QFrame::Shape enum. So inside this class the unqualified name "Shape" resolves
// to QFrame::Shape, not our annotation struct. We therefore refer to our struct
// as ::Shape throughout this header and its .cpp.

class AnnotationModel;
class ClassColors;
class QGraphicsScene;
class QGraphicsPixmapItem;
class QImage;
class QTimer;

// ImageView: the annotation canvas. It displays one image fitted to the window
// (no zoom/pan) and lets the user draw and edit shapes over it.
//
// Built on QGraphicsView because the pixmap item sits at the scene origin, so
// mapToScene(pos) converts a mouse click straight into image-pixel coordinates.
// Drawing happens in drawForeground(), whose painter is already in scene ==
// image-pixel coordinates.
//
// Drawing (creating new shapes) happens via the active Mode (Rectangle/Polygon).
// Editing committed shapes is mode-agnostic and gesture-based:
//   * long-press a committed vertex  -> drag it
//   * long-press a committed edge    -> insert a vertex there and drag it
//   * right-click a committed shape  -> context menu (delete vertex / delete shape)
// Committed shapes live in the AnnotationModel (owned by MainWindow); the canvas
// reads it to render and edits it in place, emitting annotationsChanged() so
// MainWindow can re-export.
class ImageView : public QGraphicsView
{
    Q_OBJECT

public:
    enum class Mode { Rectangle, Polygon };

    explicit ImageView(QWidget *parent = nullptr);

    void setImage(const QImage &image);
    void clear();
    bool hasImage() const;

    void setModel(AnnotationModel *model);
    void setClassColors(const ClassColors *colors);  // for per-class rendering
    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

signals:
    void shapeDrawn(const ::Shape &shape);  // new shape completed; needs a class
    void removeLastRequested();             // Delete with nothing in progress
    void annotationsChanged();              // a committed shape was edited/removed

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;

private slots:
    void onLongPress();   // long-press timer fired -> begin a vertex drag

private:
    void    fitImage();
    void    resetInteraction();
    QPointF clampToImage(const QPointF &scenePoint) const;
    double  sceneScale() const;
    void    drawShape(QPainter *painter, const ::Shape &shape, double radius) const;

    // Hit-testing against committed shapes (all in image-pixel coordinates).
    struct VertexHit { int shape = -1; int vertex = -1; };
    struct EdgeHit   { int shape = -1; int insertIndex = -1; };
    VertexHit hitVertex(const QPointF &p) const;
    EdgeHit   hitEdge(const QPointF &p) const;
    int       hitShape(const QPointF &p) const;

    void cancelLongPress();
    void showContextMenu(const QPoint &globalPos, const QPointF &scenePos);

    QGraphicsScene      *m_scene;
    QGraphicsPixmapItem *m_pixmapItem;
    AnnotationModel     *m_model       = nullptr;
    const ClassColors   *m_classColors = nullptr;

    Mode m_mode = Mode::Rectangle;

    // Drawing-new-shape state.
    bool             m_drawing = false;
    QVector<QPointF> m_current;
    QPointF          m_cursor;
    bool             m_pending = false;
    ::Shape          m_pendingShape;

    // Editing-committed-shape state.
    QTimer  *m_longPress = nullptr;
    bool     m_pressActive = false;   // a left-press over a committed feature, awaiting long-press
    QPointF  m_pressScene;
    int      m_pressShape  = -1;
    int      m_pressVertex = -1;      // >=0 when pressing a vertex
    int      m_edgeInsert  = -1;      // >=0 when pressing an edge (insert position)

    bool     m_dragging  = false;
    int      m_dragShape = -1;
    int      m_dragVertex = -1;
    ::Shape  m_dragWorking;           // live copy edited during a drag

    int      m_hoverShape  = -1;      // committed vertex under the cursor (idle)
    int      m_hoverVertex = -1;

    Q_DISABLE_COPY(ImageView)
};
