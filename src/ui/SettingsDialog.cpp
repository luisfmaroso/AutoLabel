#include "ui/SettingsDialog.h"

#include "annotation/ClassColors.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace {
constexpr int kColClassId = 0;
constexpr int kColColor   = 1;
const char    *kColorProp = "swatchColor";
} // namespace

SettingsDialog::SettingsDialog(const QHash<int, QColor> &overrides, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Settings - Class Colours"));
    setModal(true);
    resize(360, 320);

    auto *intro = new QLabel(
        tr("Assign a colour to a class id. Classes without a row use a default "
           "palette colour."),
        this);
    intro->setWordWrap(true);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({ tr("Class id"), tr("Colour") });
    m_table->horizontalHeader()->setSectionResizeMode(kColClassId, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(kColColor, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);

    // Populate from current overrides, sorted by class id for a stable view.
    QList<int> ids = overrides.keys();
    std::sort(ids.begin(), ids.end());
    for (int id : ids) {
        appendRow(id, overrides.value(id));
    }

    auto *addBtn = new QPushButton(tr("Add"), this);
    auto *delBtn = new QPushButton(tr("Remove"), this);
    connect(addBtn, &QPushButton::clicked, this, &SettingsDialog::addRow);
    connect(delBtn, &QPushButton::clicked, this, &SettingsDialog::removeSelectedRow);

    auto *rowButtons = new QHBoxLayout;
    rowButtons->addWidget(addBtn);
    rowButtons->addWidget(delBtn);
    rowButtons->addStretch(1);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
        this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
            this, &SettingsDialog::restoreDefaults);

    auto *root = new QVBoxLayout(this);
    root->addWidget(intro);
    root->addWidget(m_table, /*stretch=*/1);
    root->addLayout(rowButtons);
    root->addWidget(buttons);
}

void SettingsDialog::appendRow(int classId, const QColor &color)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *idItem = new QTableWidgetItem(QString::number(classId));
    idItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, kColClassId, idItem);

    auto *swatch = new QPushButton(this);
    const QColor c = color.isValid() ? color : ClassColors::defaultColor(classId);
    styleSwatch(swatch, c);
    connect(swatch, &QPushButton::clicked, this, [this, swatch] { pickColor(swatch); });
    m_table->setCellWidget(row, kColColor, swatch);
}

void SettingsDialog::addRow()
{
    // New rows default to the next unused class id.
    int next = 0;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        if (auto *item = m_table->item(r, kColClassId)) {
            next = std::max(next, item->text().toInt() + 1);
        }
    }
    appendRow(next, ClassColors::defaultColor(next));
}

void SettingsDialog::removeSelectedRow()
{
    const int row = m_table->currentRow();
    if (row >= 0) {
        m_table->removeRow(row);
    }
}

void SettingsDialog::pickColor(QPushButton *button)
{
    const QColor current = button->property(kColorProp).value<QColor>();
    const QColor chosen  = QColorDialog::getColor(current, this, tr("Choose class colour"));
    if (chosen.isValid()) {
        styleSwatch(button, chosen);
    }
}

void SettingsDialog::styleSwatch(QPushButton *button, const QColor &color) const
{
    button->setProperty(kColorProp, color);
    button->setText(color.name());
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; color: %2; border: 1px solid #888; "
        "min-height: 22px; }")
        .arg(color.name(), color.lightness() < 128 ? "#fff" : "#000"));
}

void SettingsDialog::restoreDefaults()
{
    // Clearing all overrides means every class falls back to the palette.
    m_table->setRowCount(0);
}

QHash<int, QColor> SettingsDialog::overrides() const
{
    QHash<int, QColor> result;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        auto *idItem = m_table->item(r, kColClassId);
        auto *swatch = qobject_cast<QPushButton *>(m_table->cellWidget(r, kColColor));
        if (!idItem || !swatch) {
            continue;
        }
        bool ok = false;
        const int id = idItem->text().toInt(&ok);
        if (ok) {
            result.insert(id, swatch->property(kColorProp).value<QColor>());  // last row wins on dup
        }
    }
    return result;
}
