#pragma once

#include <QPointF>
#include <QVector>

// Shape: one annotation on an image, stored in image-pixel coordinates.
//
//  * Rectangle -> exactly two points (two opposite corners, in any order).
//                 Exported as a YOLO *detection* line: cls cx cy w h.
//  * Polygon   -> three or more vertices. Exported as a YOLO *segmentation*
//                 line: cls x1 y1 x2 y2 ...
//
// classId is -1 until the user assigns a class. Coordinates are kept in pixels
// (not normalized) so rendering is trivial; normalization happens only at
// export time, where the image size is known.
struct Shape
{
    enum class Type { Rectangle, Polygon };

    Type             type    = Type::Rectangle;
    QVector<QPointF> points;
    int              classId = -1;

    bool isComplete() const
    {
        return type == Type::Rectangle ? points.size() == 2
                                       : points.size() >= 3;
    }
};
