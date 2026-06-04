#include "annotation/AnnotationModel.h"

AnnotationModel::AnnotationModel(QObject *parent)
    : QObject(parent)
{
}

void AnnotationModel::addShape(const Shape &shape)
{
    m_shapes.append(shape);
    emit changed();
}

void AnnotationModel::removeLast()
{
    if (!m_shapes.isEmpty()) {
        m_shapes.removeLast();
        emit changed();
    }
}

void AnnotationModel::removeAt(int index)
{
    if (index >= 0 && index < m_shapes.size()) {
        m_shapes.removeAt(index);
        emit changed();
    }
}

void AnnotationModel::updateShape(int index, const Shape &shape)
{
    if (index >= 0 && index < m_shapes.size()) {
        m_shapes[index] = shape;
        emit changed();
    }
}

void AnnotationModel::setShapes(const QVector<Shape> &shapes)
{
    m_shapes = shapes;
    emit changed();
}

void AnnotationModel::clear()
{
    if (!m_shapes.isEmpty()) {
        m_shapes.clear();
        emit changed();
    }
}
