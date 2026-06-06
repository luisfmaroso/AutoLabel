#include "app/MainWindow.h"

#include "annotation/AnnotationModel.h"
#include "annotation/YoloIo.h"
#include "backend/SamBackend.h"
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
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QMessageBox>
#include <QRectF>
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
    , m_sam(new SamBackend(this))
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
    connect(m_view, &ImageView::samPromptsChanged, this, [this] {
        const int n = m_view->samPoints().size();
        if (n > 0) {
            statusBar()->showMessage(tr("%1 prompt point(s). SAM -> Generate Mask (G).").arg(n), 3000);
        }
    });

    connect(m_sam, &SamBackend::responseReceived, this, &MainWindow::onSamResponse);
    connect(m_sam, &SamBackend::started, this, [this] {
        statusBar()->showMessage(tr("SAM backend started."), 4000);
        if (m_pingSamAction) {
            m_pingSamAction->setEnabled(true);
        }
    });
    connect(m_sam, &SamBackend::stopped, this, [this] {
        statusBar()->showMessage(tr("SAM backend stopped."), 4000);
        if (m_pingSamAction) {
            m_pingSamAction->setEnabled(false);
        }
    });
    connect(m_sam, &SamBackend::errorOccurred, this, [this](const QString &message) {
        statusBar()->showMessage(tr("SAM backend: %1").arg(message), 6000);
    });

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

    auto *samModeAction = annotateMenu->addAction(tr("&SAM (point prompts)"));
    samModeAction->setCheckable(true);
    samModeAction->setShortcut(QKeySequence(tr("S")));
    modeGroup->addAction(samModeAction);
    connect(samModeAction, &QAction::triggered, this, [this] {
        m_view->setMode(ImageView::Mode::Sam);
        statusBar()->showMessage(
            tr("SAM mode: left-click = positive point, right-click = negative. "
               "Then SAM -> Generate Mask."), 6000);
    });

    // --- SAM (Python backend) ------------------------------------------
    auto *samMenu = menuBar()->addMenu(tr("SA&M"));
    auto *startSamAction = samMenu->addAction(tr("&Start Backend"));
    connect(startSamAction, &QAction::triggered, this, &MainWindow::startSamBackend);

    m_pingSamAction = samMenu->addAction(tr("&Ping Backend"));
    m_pingSamAction->setEnabled(false);  // enabled once the backend starts
    connect(m_pingSamAction, &QAction::triggered, this, &MainWindow::pingSamBackend);

    auto *loadModelAction = samMenu->addAction(tr("&Load SAM2 Model"));
    connect(loadModelAction, &QAction::triggered, this, &MainWindow::loadSamModel);

    samMenu->addSeparator();

    auto *generateAction = samMenu->addAction(tr("&Generate Mask"));
    generateAction->setShortcut(QKeySequence(tr("G")));
    connect(generateAction, &QAction::triggered, this, &MainWindow::generateMask);

    auto *acceptAction = samMenu->addAction(tr("&Accept Mask"));
    connect(acceptAction, &QAction::triggered, m_view, &ImageView::acceptPendingShape);

    auto *clearPointsAction = samMenu->addAction(tr("&Clear Points"));
    clearPointsAction->setShortcut(QKeySequence(tr("C")));
    connect(clearPointsAction, &QAction::triggered, m_view, &ImageView::clearSamPrompts);

    samMenu->addSeparator();

    auto *seedMemoryAction = samMenu->addAction(tr("Load Polygon to SAM &Memory"));
    seedMemoryAction->setShortcut(QKeySequence(tr("M")));
    connect(seedMemoryAction, &QAction::triggered, this, &MainWindow::loadPolygonToMemory);

    auto *predictAction = samMenu->addAction(tr("Predict &This Frame (Space)"));
    predictAction->setShortcut(QKeySequence(Qt::Key_Space));
    connect(predictAction, &QAction::triggered, this, &MainWindow::predictThisFrame);

    auto *resetTrackAction = samMenu->addAction(tr("&Reset Tracking"));
    connect(resetTrackAction, &QAction::triggered, this, &MainWindow::resetTracking);

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

    // A new sequence starts fresh tracking.
    m_memorySeeded = false;
    if (m_sam->isRunning()) {
        m_sam->resetTracking();
    }

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

void MainWindow::startSamBackend()
{
    if (m_sam->isRunning()) {
        statusBar()->showMessage(tr("SAM backend already running."), 3000);
        return;
    }
    statusBar()->showMessage(tr("Starting SAM backend..."), 3000);
    m_sam->start();
}

void MainWindow::pingSamBackend()
{
    statusBar()->showMessage(tr("Pinging SAM backend..."), 2000);
    m_sam->ping();
}

void MainWindow::loadSamModel()
{
    if (!m_sam->isRunning()) {
        statusBar()->showMessage(tr("Start the SAM backend first."), 4000);
        return;
    }

    // Resolve the submodule + checkpoint relative to the repo root (the .exe
    // lives in build/bin), with QSettings overrides for non-standard layouts.
    QDir repo(QApplication::applicationDirPath());
    repo.cdUp();   // build/bin -> build
    repo.cdUp();   // build     -> repo root

    QSettings settings;
    const QString sam2Root = settings.value(QStringLiteral("sam2Root"),
        repo.absoluteFilePath(QStringLiteral("third_party/sam2"))).toString();
    const QString checkpoint = settings.value(QStringLiteral("sam2Checkpoint"),
        repo.absoluteFilePath(QStringLiteral("models/sam2.1_hiera_tiny.pt"))).toString();
    const QString config = settings.value(QStringLiteral("sam2Config"),
        QStringLiteral("configs/sam2.1/sam2.1_hiera_t.yaml")).toString();
    const QString device = settings.value(QStringLiteral("samDevice"),
        QStringLiteral("cpu")).toString();

    statusBar()->showMessage(tr("Loading SAM2 model (this can take a few seconds)..."));
    m_sam->loadModel(checkpoint, config, sam2Root, device);
}

void MainWindow::generateMask()
{
    if (m_currentIndex < 0) {
        statusBar()->showMessage(tr("Open an image first."), 4000);
        return;
    }
    if (!m_sam->isRunning()) {
        statusBar()->showMessage(tr("Start the SAM backend and load the model first."), 5000);
        return;
    }
    const QList<QPointF> points = m_view->samPoints();
    const QList<int>     labels = m_view->samLabels();
    if (points.isEmpty()) {
        statusBar()->showMessage(
            tr("Switch to SAM mode and place at least one positive point."), 5000);
        return;
    }
    statusBar()->showMessage(tr("Generating mask (%1 point(s))...").arg(points.size()));
    m_sam->segment(m_imagePaths.at(m_currentIndex), points, labels);
}

void MainWindow::loadPolygonToMemory()
{
    if (!m_sam->isRunning()) {
        statusBar()->showMessage(tr("Start the SAM backend and load the model first."), 5000);
        return;
    }
    if (m_currentIndex < 0 || m_imagePaths.size() < 2) {
        statusBar()->showMessage(tr("Open a folder of frames first."), 5000);
        return;
    }
    if (m_annotations->isEmpty()) {
        statusBar()->showMessage(
            tr("Draw the object's polygon on this frame first, then load it to memory (M)."), 6000);
        return;
    }

    // Use the last committed shape as the object; its class is applied to predictions.
    const Shape seed = m_annotations->shapes().last();
    QList<QPointF> polygon;
    if (seed.type == Shape::Type::Rectangle && seed.points.size() == 2) {
        const QRectF r = QRectF(seed.points[0], seed.points[1]).normalized();
        polygon = { r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft() };
    } else {
        polygon = QList<QPointF>(seed.points.begin(), seed.points.end());
    }
    if (polygon.size() < 3) {
        statusBar()->showMessage(tr("Seed shape is too small."), 5000);
        return;
    }

    m_trackClassId  = seed.classId >= 0 ? seed.classId : m_lastClassId;
    m_memorySeeded  = true;
    m_sam->seedMemory(m_imagePaths, m_currentIndex, polygon);
    statusBar()->showMessage(
        tr("Loading polygon to SAM memory... then go to the next frame and press Space."), 5000);
}

void MainWindow::predictThisFrame()
{
    if (!m_sam->isRunning()) {
        statusBar()->showMessage(tr("Start the SAM backend and load the model first."), 5000);
        return;
    }
    if (!m_memorySeeded) {
        statusBar()->showMessage(
            tr("Load a polygon to SAM memory first (draw it, then press M)."), 6000);
        return;
    }
    statusBar()->showMessage(tr("Predicting frame %1...").arg(m_currentIndex + 1));
    m_sam->predictFrame(m_currentIndex);
}

void MainWindow::resetTracking()
{
    m_memorySeeded = false;
    if (m_sam->isRunning()) {
        m_sam->resetTracking();
    }
    statusBar()->showMessage(tr("SAM memory cleared. Draw a new polygon and press M."), 4000);
}

void MainWindow::onSamResponse(const QJsonObject &reply)
{
    const QString command = reply.value(QStringLiteral("command")).toString();
    const QString status  = reply.value(QStringLiteral("status")).toString();
    const QString message = reply.value(QStringLiteral("message")).toString();

    if (command == QStringLiteral("track_seed")) {
        statusBar()->showMessage(
            status == QStringLiteral("ok")
                ? tr("Polygon loaded to SAM memory. Go to the next frame and press Space.")
                : tr("Could not load to memory: %1").arg(message), 6000);
        return;
    }

    if (command == QStringLiteral("track_step")) {
        if (status != QStringLiteral("ok")) {
            statusBar()->showMessage(tr("Predict failed: %1").arg(message), 6000);
            return;
        }
        const int frame = reply.value(QStringLiteral("frame")).toInt();
        const QSize size(reply.value(QStringLiteral("width")).toInt(),
                         reply.value(QStringLiteral("height")).toInt());

        Shape s;
        s.type = Shape::Type::Polygon;
        s.classId = m_trackClassId;
        for (const QJsonValue &pt : reply.value(QStringLiteral("polygon")).toArray()) {
            const QJsonArray xy = pt.toArray();
            if (xy.size() == 2) {
                s.points.append(QPointF(xy[0].toDouble(), xy[1].toDouble()));
            }
        }

        if (!s.isComplete()) {
            statusBar()->showMessage(
                tr("SAM lost the object on this frame - correct it by hand, or Reset Tracking "
                   "and re-seed (M)."), 7000);
            return;
        }

        // Apply the predicted polygon to the frame it belongs to (single-object
        // tracking: it replaces that frame's annotation).
        if (frame == m_currentIndex) {
            m_annotations->setShapes(QVector<Shape>{ s });
            saveAnnotations();
        } else if (frame >= 0 && frame < m_imagePaths.size()) {
            YoloIo::writeFile(labelPathForIndex(frame), QVector<Shape>{ s }, size);
        }
        statusBar()->showMessage(
            tr("Predicted frame %1. If it's good, go to the next frame and press Space; "
               "if not, fix it or Reset Tracking.").arg(frame + 1), 6000);
        return;
    }

    if (command == QStringLiteral("segment")) {
        if (status != QStringLiteral("ok")) {
            statusBar()->showMessage(tr("SAM segment failed: %1").arg(message), 6000);
            return;
        }
        // Use the largest returned polygon as the preview (first; sorted by area).
        const QJsonArray polygons = reply.value(QStringLiteral("polygons")).toArray();
        if (polygons.isEmpty()) {
            statusBar()->showMessage(tr("SAM returned no polygon; try another point."), 5000);
            return;
        }
        QList<QPointF> poly;
        for (const QJsonValue &pt : polygons.first().toArray()) {
            const QJsonArray xy = pt.toArray();
            if (xy.size() == 2) {
                poly.append(QPointF(xy[0].toDouble(), xy[1].toDouble()));
            }
        }
        m_view->setSamPreview(poly);
        statusBar()->showMessage(
            tr("Mask ready (score %1). Press Enter to accept, or add points and regenerate.")
                .arg(reply.value(QStringLiteral("score")).toDouble(), 0, 'f', 2), 8000);
        return;
    }

    // ping / load_model / others.
    statusBar()->showMessage(tr("SAM response: %1 (%2)").arg(status, message), 5000);
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
    return labelPathForIndex(m_currentIndex);
}

QString MainWindow::labelPathForIndex(int index) const
{
    const QFileInfo info(m_imagePaths.at(index));
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
