#pragma once

#include <QColor>
#include <QHash>

class QSettings;

// ClassColors: maps a class id to the colour used to draw its shapes.
//
// Every class id always has a colour: if the user hasn't overridden it, a
// deterministic default is taken from a fixed palette (classId % paletteSize),
// so classes look distinct out of the box. The Settings dialog edits the
// overrides; MainWindow persists them via QSettings. Plain value type (not a
// QObject) -- MainWindow owns one and hands the canvas a pointer for rendering.
class ClassColors
{
public:
    QColor colorFor(int classId) const;

    void setOverride(int classId, const QColor &color);
    void setOverrides(const QHash<int, QColor> &overrides) { m_overrides = overrides; }
    const QHash<int, QColor> &overrides() const { return m_overrides; }

    void load(QSettings &settings);
    void save(QSettings &settings) const;

    static QColor defaultColor(int classId);

private:
    QHash<int, QColor> m_overrides;
};
