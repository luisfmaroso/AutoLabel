#include "app/MainWindow.h"

#include "annotation/AnnotationModel.h"
#include "annotation/YoloIo.h"
#include "ui/ImageView.h"
#include "ui/SettingsDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>

namespace {

// Extensions we offer to open. Kept in sync with the QFileDialog filter below.
const QStringList kImageExtensions = {
    "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif",
    "*.tif", "*.tiff", "*.webp"
};

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_view(new ImageView)
    , m_annotations(new AnnotationModel(this))
{
    setWindowTitle(tr("AutoLabel"));
    resize(1200, 720);

    QSettings settings;
    m_classColors.load(settings);

    m_view->setModel(m_annotations);
    m_view->setClassColors(&m_classColors);

    buildCentralWidget();
    buildMenus();
    updateNavigationActions();

    connect(m_view, &ImageView::shapeDrawn, this, &MainWindow::onShapeDrawn);
    connect(m_view, &ImageView::removeLastRequested, this, &MainWindow::onRemoveLastRequested);
    connect(m_view, &ImageView::annotationsChanged, this, &MainWindow::saveAnnotations);

    statusBar()->showMessage(tr("Ready. Open an image folder to begin."));
}

void MainWindow::buildCentralWidget()
{
    setCentralWidget(m_view);
}

void MainWindow::buildMenus()
{
    // --- File ----------------------------------------------------------
    auto *fileMenu = menuBar()->addMenu(tr("&File"));

    auto *openFolderAction = fileMenu->addAction(tr("&Open Image Folder..."));
    openFolderAction->setShortcut(QKeySequence::Open);
    connect(openFolderAction, &QAction::triggered, this, &MainWindow::openImageFolder);

    fileMenu->addSeparator();

    auto *exitAction = fileMenu->addAction(tr("E&xit"));
    exitAction->setShortcut(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

    // --- Image (navigation) --------------------------------------------
    auto *imageMenu = menuBar()->addMenu(tr("&Image"));

    m_prevAction = imageMenu->addAction(tr("&Previous Image"));
    m_prevAction->setShortcut(QKeySequence(Qt::Key_Left));
    connect(m_prevAction, &QAction::triggered, this, &MainWindow::previousImage);

    m_nextAction = imageMenu->addAction(tr("&Next Image"));
    m_nextAction->setShortcut(QKeySequence(Qt::Key_Right));
    connect(m_nextAction, &QAction::triggered, this, &MainWindow::nextImage);

    // --- Annotate (drawing mode) ---------------------------------------
    auto *annotateMenu = menuBar()->addMenu(tr("&Annotate"));

    auto *modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);

    auto *rectAction = annotateMenu->addAction(tr("&Rectangle (box)"));
    rectAction->setCheckable(true);
    rectAction->setChecked(true);
    rectAction->setShortcut(QKeySequence(tr("R")));
    modeGroup->addAction(rectAction);
    connect(rectAction, &QAction::triggered, this, [this] {
        m_view->setMode(ImageView::Mode::Rectangle);
        statusBar()->showMessage(tr("Rectangle mode: click two opposite corners."), 4000);
    });

    auto *polyAction = annotateMenu->addAction(tr("&Polygon (segmentation)"));
    polyAction->setCheckable(true);
    polyAction->setShortcut(QKeySequence(tr("P")));
    modeGroup->addAction(polyAction);
    connect(polyAction, &QAction::triggered, this, [this] {
        m_view->setMode(ImageView::Mode::Polygon);
        statusBar()->showMessage(
            tr("Polygon mode: click vertices; click near the first to close."), 4000);
    });

    // --- Settings ------------------------------------------------------
    auto *settingsMenu = menuBar()->addMenu(tr("&Settings"));
    auto *colorsAction = settingsMenu->addAction(tr("&Class Colours..."));
    colorsAction->setShortcut(QKeySequence(tr("Ctrl+,")));
    connect(colorsAction, &QAction::triggered, this, &MainWindow::openSettings);

    // --- Help ----------------------------------------------------------
    auto *helpMenu = menuBar()->addMenu(tr("&Help"));

    auto *aboutAction = helpMenu->addAction(tr("&About AutoLabel"));
    connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::openImageFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open Image Folder"), QString(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (dir.isEmpty()) {
        return;
    }

    const QFileInfoList entries =
        QDir(dir).entryInfoList(kImageExtensions, QDir::Files, QDir::Name);

    QStringList paths;
    paths.reserve(entries.size());
    for (const QFileInfo &info : entries) {
        paths.append(info.absoluteFilePath());
    }

    if (paths.isEmpty()) {
        m_imagePaths.clear();
        m_currentIndex = -1;
        m_annotations->clear();
        m_view->clear();
        setWindowTitle(tr("AutoLabel"));
        updateNavigationActions();
        QMessageBox::information(
            this, tr("No Images Found"),
            tr("No supported image files were found in:\n%1").arg(dir));
        return;
    }

    m_imagePaths   = paths;
    m_currentIndex = 0;
    loadCurrentImage();
    statusBar()->showMessage(
        tr("Opened %1 (%2 image(s)).").arg(QDir(dir).dirName()).arg(paths.size()), 5000);
}

void MainWindow::loadCurrentImage()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_imagePaths.size()) {
        return;
    }

    const QString path = m_imagePaths.at(m_currentIndex);

    QImageReader reader(path);
    reader.setAutoTransform(true);  // honour EXIF orientation
    const QImage image = reader.read();

    if (image.isNull()) {
        QMessageBox::warning(
            this, tr("Cannot Open Image"),
            tr("Failed to read:\n%1\n\n%2").arg(path, reader.errorString()));
        m_view->clear();
        m_annotations->clear();
        m_currentImageSize = QSize();
        updateNavigationActions();
        return;
    }

    m_currentImageSize = image.size();
    m_view->setImage(image);

    // Load any existing labels for this image so prior work is shown / editable.
    m_annotations->setShapes(YoloIo::readFile(labelPathForCurrentImage(), m_currentImageSize));

    const QFileInfo info(path);
    setWindowTitle(tr("AutoLabel - %1").arg(info.fileName()));
    statusBar()->showMessage(
        tr("%1  [%2/%3]  %4 x %5  -  %6 shape(s)")
            .arg(info.fileName())
            .arg(m_currentIndex + 1)
            .arg(m_imagePaths.size())
            .arg(image.width())
            .arg(image.height())
            .arg(m_annotations->count()));

    m_view->setFocus();  // so Enter/Esc/Delete reach the canvas immediately
    updateNavigationActions();
}

void MainWindow::onShapeDrawn(const Shape &shape)
{
    bool ok = false;
    const int classId = QInputDialog::getInt(
        this, tr("Assign Class"), tr("Class id:"),
        m_lastClassId, 0, 9999, 1, &ok);
    if (!ok) {
        return;  // user cancelled -> shape discarded
    }

    m_lastClassId = classId;
    Shape committed = shape;
    committed.classId = classId;
    m_annotations->addShape(committed);
    saveAnnotations();

    statusBar()->showMessage(
        tr("Added shape (class %1). %2 shape(s) total.")
            .arg(classId).arg(m_annotations->count()), 4000);
}

void MainWindow::onRemoveLastRequested()
{
    if (m_annotations->isEmpty()) {
        return;
    }
    m_annotations->removeLast();
    saveAnnotations();
    statusBar()->showMessage(
        tr("Removed last shape. %1 shape(s) remaining.").arg(m_annotations->count()), 4000);
}

void MainWindow::openSettings()
{
    SettingsDialog dialog(m_classColors.overrides(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_classColors.setOverrides(dialog.overrides());

    QSettings settings;
    m_classColors.save(settings);

    m_view->setClassColors(&m_classColors);  // triggers a repaint with new colours
    statusBar()->showMessage(tr("Class colours updated."), 3000);
}

void MainWindow::saveAnnotations() const
{
    if (m_currentIndex < 0 || m_currentImageSize.isEmpty()) {
        return;
    }
    YoloIo::writeFile(labelPathForCurrentImage(), m_annotations->shapes(), m_currentImageSize);
}

QString MainWindow::labelPathForCurrentImage() const
{
    const QFileInfo info(m_imagePaths.at(m_currentIndex));
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".txt");
}

void MainWindow::nextImage()
{
    if (m_currentIndex >= 0 && m_currentIndex + 1 < m_imagePaths.size()) {
        ++m_currentIndex;
        loadCurrentImage();
    }
}

void MainWindow::previousImage()
{
    if (m_currentIndex > 0) {
        --m_currentIndex;
        loadCurrentImage();
    }
}

void MainWindow::updateNavigationActions()
{
    const bool hasImages = !m_imagePaths.isEmpty();
    m_prevAction->setEnabled(m_currentIndex > 0);
    m_nextAction->setEnabled(hasImages && m_currentIndex + 1 < m_imagePaths.size());
}

void MainWindow::showAbout()
{
    QMessageBox::about(
        this,
        tr("About AutoLabel"),
        tr("<b>AutoLabel</b> %1<br><br>"
           "A lightweight image annotation tool for building YOLO segmentation "
           "datasets with Meta SAM2.<br><br>"
           "Part of the same software family as InferenceVisualizer.")
            .arg(QApplication::applicationVersion()));
}
