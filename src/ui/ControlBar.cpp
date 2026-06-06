#include "ui/ControlBar.h"

#include <QAction>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QToolButton>

namespace {

QToolButton *makeButton(QAction *action, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setDefaultAction(action);                 // text/checked/enabled stay in sync
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setAutoRaise(true);
    return button;
}

QFrame *makeSeparator(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFrameShape(QFrame::VLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

} // namespace

ControlBar::ControlBar(const Actions &actions, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(4);

    // Navigation + frame counter.
    layout->addWidget(makeButton(actions.prev, this));
    m_counter = new QLabel(QStringLiteral("- / -"), this);
    m_counter->setAlignment(Qt::AlignCenter);
    m_counter->setMinimumWidth(70);
    layout->addWidget(m_counter);
    layout->addWidget(makeButton(actions.next, this));

    layout->addWidget(makeSeparator(this));

    // Drawing modes + accept.
    layout->addWidget(makeButton(actions.rectMode, this));
    layout->addWidget(makeButton(actions.polyMode, this));
    layout->addWidget(makeButton(actions.accept, this));

    layout->addWidget(makeSeparator(this));

    // SAM tracking.
    layout->addWidget(makeButton(actions.loadMemory, this));
    layout->addWidget(makeButton(actions.predict, this));
    layout->addWidget(makeButton(actions.resetTracking, this));

    layout->addStretch(1);

    // SAM status indicator (right-aligned).
    m_status = new QLabel(this);
    layout->addWidget(m_status);
    setSamStatus(tr("off"), QColor(0x88, 0x88, 0x88));
}

void ControlBar::setFrameCounter(const QString &text)
{
    m_counter->setText(text);
}

void ControlBar::setSamStatus(const QString &text, const QColor &colour)
{
    m_status->setText(tr("SAM: %1").arg(text));
    m_status->setStyleSheet(QStringLiteral("QLabel { color: %1; font-weight: 600; }")
                                .arg(colour.name()));
}
