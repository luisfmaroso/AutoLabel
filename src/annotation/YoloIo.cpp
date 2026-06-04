#include "annotation/YoloIo.h"

#include <QFile>
#include <QTextStream>

#include <algorithm>

namespace {

// Format a normalized coordinate with 6 decimals (YOLO convention).
QString num(double v)
{
    return QString::number(v, 'f', 6);
}

QString rectangleLine(const Shape &s, const QSize &size)
{
    // Two opposite corners in any order -> min/max box.
    const double x0 = std::min(s.points[0].x(), s.points[1].x());
    const double y0 = std::min(s.points[0].y(), s.points[1].y());
    const double x1 = std::max(s.points[0].x(), s.points[1].x());
    const double y1 = std::max(s.points[0].y(), s.points[1].y());

    const double w = size.width();
    const double h = size.height();
    const double cx = ((x0 + x1) / 2.0) / w;
    const double cy = ((y0 + y1) / 2.0) / h;
    const double bw = (x1 - x0) / w;
    const double bh = (y1 - y0) / h;

    return QStringLiteral("%1 %2 %3 %4 %5")
        .arg(s.classId)
        .arg(num(cx), num(cy), num(bw), num(bh));
}

QString polygonLine(const Shape &s, const QSize &size)
{
    const double w = size.width();
    const double h = size.height();

    QString line = QString::number(s.classId);
    for (const QPointF &p : s.points) {
        line += QLatin1Char(' ') + num(p.x() / w) + QLatin1Char(' ') + num(p.y() / h);
    }
    return line;
}

} // namespace

namespace YoloIo {

QString toText(const QVector<Shape> &shapes, const QSize &imageSize)
{
    if (imageSize.isEmpty()) {
        return QString();
    }

    QString text;
    for (const Shape &s : shapes) {
        if (s.classId < 0 || !s.isComplete()) {
            continue;
        }
        text += (s.type == Shape::Type::Rectangle ? rectangleLine(s, imageSize)
                                                   : polygonLine(s, imageSize));
        text += QLatin1Char('\n');
    }
    return text;
}

bool writeFile(const QString &path, const QVector<Shape> &shapes, const QSize &imageSize)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return false;
    }
    QTextStream out(&file);
    out << toText(shapes, imageSize);
    return true;
}

QVector<Shape> readFile(const QString &path, const QSize &imageSize)
{
    QVector<Shape> shapes;
    if (imageSize.isEmpty()) {
        return shapes;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return shapes;  // no labels yet -- not an error
    }

    const double w = imageSize.width();
    const double h = imageSize.height();

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QStringList tok = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (tok.size() < 5) {
            continue;  // malformed
        }

        Shape s;
        s.classId = tok[0].toInt();

        if (tok.size() == 5) {
            // Detection: cls cx cy w h -> two corner points in pixels.
            const double cx = tok[1].toDouble() * w;
            const double cy = tok[2].toDouble() * h;
            const double bw = tok[3].toDouble() * w;
            const double bh = tok[4].toDouble() * h;
            s.type = Shape::Type::Rectangle;
            s.points = { QPointF(cx - bw / 2.0, cy - bh / 2.0),
                         QPointF(cx + bw / 2.0, cy + bh / 2.0) };
        } else {
            // Segmentation: cls x1 y1 x2 y2 ... (needs an even number of coords).
            const int coords = tok.size() - 1;
            if (coords % 2 != 0) {
                continue;
            }
            s.type = Shape::Type::Polygon;
            for (int i = 1; i + 1 < tok.size(); i += 2) {
                s.points.append(QPointF(tok[i].toDouble() * w, tok[i + 1].toDouble() * h));
            }
        }

        if (s.isComplete()) {
            shapes.append(s);
        }
    }
    return shapes;
}

} // namespace YoloIo
