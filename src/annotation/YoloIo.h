#pragma once

#include "annotation/Shape.h"

#include <QSize>
#include <QString>
#include <QVector>

// YoloIo: conversion between Shape lists and YOLO label files. Free functions,
// no state -- the one place that knows the on-disk format, so MainWindow and
// the model stay format-agnostic.
//
// Coordinates are normalized to [0, 1] by the image size. A rectangle becomes a
// YOLO detection line (cls cx cy w h); a polygon becomes a YOLO segmentation
// line (cls x1 y1 x2 y2 ...). On read, a 5-token line is treated as a rectangle
// and any longer odd-token line as a polygon.
namespace YoloIo {

// Serialize shapes to the YOLO text body (one line per shape, trailing newline).
QString toText(const QVector<Shape> &shapes, const QSize &imageSize);

// Write the YOLO file. Returns false (and leaves no file) on I/O error.
bool writeFile(const QString &path, const QVector<Shape> &shapes, const QSize &imageSize);

// Parse a YOLO file back into shapes (pixel coordinates). Unreadable or empty
// files yield an empty list.
QVector<Shape> readFile(const QString &path, const QSize &imageSize);

} // namespace YoloIo
