#pragma once

#include <QColor>
#include <QDialog>
#include <QHash>

class QTableWidget;
class QPushButton;

// SettingsDialog: a modal dialog for editing per-class colours. Mirrors
// InferenceVisualizer's SettingsDialog approach (swatch buttons + QColorDialog,
// Ok/Cancel/RestoreDefaults). It edits a local copy of the overrides and reports
// back via overrides() on accept; MainWindow persists and applies them.
//
// The table lists class id -> colour rows. Class ids without a row fall back to
// the deterministic default palette, so the user only needs to add rows for the
// classes whose colour they want to pin.
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(const QHash<int, QColor> &overrides, QWidget *parent = nullptr);

    QHash<int, QColor> overrides() const;

private slots:
    void addRow();
    void removeSelectedRow();
    void restoreDefaults();

private:
    void appendRow(int classId, const QColor &color);
    void pickColor(QPushButton *button);
    void styleSwatch(QPushButton *button, const QColor &color) const;

    QTableWidget *m_table = nullptr;
};
