#include "annotation/ClassColors.h"

#include <QSettings>

namespace {

// A fixed palette of visually distinct colours. Class ids cycle through it when
// the user hasn't set an explicit override.
const QColor kPalette[] = {
    QColor(0xe6, 0x19, 0x4b),  // red
    QColor(0x3c, 0xb4, 0x4b),  // green
    QColor(0x43, 0x63, 0xd8),  // blue
    QColor(0xf5, 0x82, 0x31),  // orange
    QColor(0x91, 0x1e, 0xb4),  // purple
    QColor(0x42, 0xd4, 0xf4),  // cyan
    QColor(0xf0, 0x32, 0xe6),  // magenta
    QColor(0xbf, 0xef, 0x45),  // lime
    QColor(0xfa, 0xbe, 0xd4),  // pink
    QColor(0x46, 0x99, 0x90),  // teal
    QColor(0x9a, 0x63, 0x24),  // brown
    QColor(0xff, 0xe1, 0x19),  // yellow
};
constexpr int kPaletteSize = int(sizeof(kPalette) / sizeof(kPalette[0]));

} // namespace

QColor ClassColors::defaultColor(int classId)
{
    const int i = (classId % kPaletteSize + kPaletteSize) % kPaletteSize;  // handle negatives
    return kPalette[i];
}

QColor ClassColors::colorFor(int classId) const
{
    const auto it = m_overrides.constFind(classId);
    return it != m_overrides.constEnd() ? it.value() : defaultColor(classId);
}

void ClassColors::setOverride(int classId, const QColor &color)
{
    if (color.isValid()) {
        m_overrides.insert(classId, color);
    } else {
        m_overrides.remove(classId);
    }
}

void ClassColors::load(QSettings &settings)
{
    m_overrides.clear();
    settings.beginGroup(QStringLiteral("classColors"));
    const QStringList keys = settings.childKeys();
    for (const QString &key : keys) {
        const QColor c(settings.value(key).toString());
        bool ok = false;
        const int id = key.toInt(&ok);
        if (ok && c.isValid()) {
            m_overrides.insert(id, c);
        }
    }
    settings.endGroup();
}

void ClassColors::save(QSettings &settings) const
{
    settings.beginGroup(QStringLiteral("classColors"));
    settings.remove(QString());  // clear stale entries in this group
    for (auto it = m_overrides.constBegin(); it != m_overrides.constEnd(); ++it) {
        settings.setValue(QString::number(it.key()), it.value().name());
    }
    settings.endGroup();
}
