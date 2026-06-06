#pragma once

#include <QWidget>

class QAction;
class QLabel;

// ControlBar: the on-screen control strip along the bottom of the main window.
// It mirrors InferenceVisualizer's VideoControls "bottom controls widget" idea:
// a thin presentational widget that surfaces the app's core actions as buttons.
//
// It does NOT own behaviour -- it shows QToolButtons backed by MainWindow's shared
// QActions (button->setDefaultAction), so a button's text/checked/enabled state
// stays in sync with the same action used by menus and keyboard shortcuts.
class ControlBar : public QWidget
{
    Q_OBJECT

public:
    // The actions the bar surfaces, grouped left-to-right. Owned by MainWindow.
    struct Actions
    {
        QAction *prev          = nullptr;
        QAction *next          = nullptr;
        QAction *rectMode      = nullptr;
        QAction *polyMode      = nullptr;
        QAction *accept        = nullptr;
        QAction *loadMemory    = nullptr;
        QAction *predict       = nullptr;
        QAction *resetTracking = nullptr;
    };

    explicit ControlBar(const Actions &actions, QWidget *parent = nullptr);

    void setFrameCounter(const QString &text);
    void setSamStatus(const QString &text, const QColor &colour);

private:
    QLabel *m_counter = nullptr;
    QLabel *m_status  = nullptr;
};
