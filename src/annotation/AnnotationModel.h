#pragma once

#include "annotation/Shape.h"

#include <QObject>
#include <QVector>

// AnnotationModel: the list of committed shapes for the image currently shown.
// Pure data + change notification -- it knows nothing about rendering, files,
// or YOLO formatting. The canvas reads it to draw; MainWindow mutates it and
// drives export. This keeps UI and business logic separated (and matches the
// "store all prompts in an annotation model" requirement).
class AnnotationModel : public QObject
{
    Q_OBJECT

public:
    explicit AnnotationModel(QObject *parent = nullptr);

    const QVector<Shape> &shapes() const { return m_shapes; }
    bool                  isEmpty() const { return m_shapes.isEmpty(); }
    int                   count() const { return m_shapes.size(); }

    void addShape(const Shape &shape);
    void removeLast();
    void removeAt(int index);
    void updateShape(int index, const Shape &shape);   // replace shape in place (editing)
    void setShapes(const QVector<Shape> &shapes);       // bulk replace (e.g. on load)
    void clear();

signals:
    void changed();

private:
    QVector<Shape> m_shapes;
};
