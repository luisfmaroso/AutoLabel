#include "app/MainWindow.h"

#include "annotation/AnnotationModel.h"
#include "annotation/YoloIo.h"
#include "backend/SamBackend.h"
#include "ui/ControlBar.h"
#include "ui/ImageView.h"
#include "ui/SettingsDialog.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
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
#include <QVBoxLayout>
#include <QWidget>

namespace {

// Extensions we offer to open. Kept in sync with the QFileDialog filter below.
const QStringList kImageExtensions = {
    "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.gif",
    "*.tif", "*.tiff", "*.webp"
};

// The selectable SAM 2.1 model variants (checkpoint file in models/, matching
// config from the submodule). Index 0 (Tiny) is the default. Larger variants are
// more accurate but much slower on CPU, and must be downloaded into models/.
struct SamModel {
    const char *name;
    const char *checkpoint;
    const char *config;
};
const SamModel kSamModels[] = {
    { "Tiny",  "sam2.1_hiera_tiny.pt",       "configs/sam2.1/sam2.1_hiera_t.yaml"  },
    { "Small", "sam2.1_hiera_small.pt",      "configs/sam2.1/sam2.1_hiera_s.yaml"  },
    { "Base+", "sam2.1_hiera_base_plus.pt",  "configs/sam2.1/sam2.1_hiera_b+.yaml" },
    { "Large", "sam2.1_hiera_large.pt",      "configs/sam2.1/sam2.1_hiera_l.yaml"  },
};
constexpr int kSamModelCount = int(sizeof(kSamModels) / sizeof(kSamModels[0]));

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
    m_modelIndex = qBound(0, settings.value(QStringLiteral("samModelIndex"), 0).toInt(),
                          kSamModelCount - 1);

    m_view->setModel(m_annotations);
    m_view->setClassColors(&m_classColors);

    buildActions();
    buildCentralWidget();
    buildMenus();
    updateNavigationActions();

    connect(m_view, &ImageView::shapeDrawn, this, &MainWindow::onShapeDrawn);
    connect(m_view, &ImageView::removeLastRequested, this, &MainWindow::onRemoveLastRequested);
    connect(m_view, &ImageView::annotationsChanged, this, &MainWindow::saveAnnotations);

    connect(m_sam, &SamBackend::responseReceived, this, &MainWindow::onSamResponse);
    connect(m_sam, &SamBackend::started, this, [this] {
        m_controlBar->setSamStatus(tr("loading model..."), QColor(0xd0, 0x8a, 0x1a));
        statusBar()->showMessage(tr("SAM backend started; loading model..."), 4000);
        loadSamModel();  // auto-load the model once the process is up
    });
    connect(m_sam, &SamBackend::stopped, this, [this] {
        setSamReady(false);
        m_controlBar->setSamStatus(tr("stopped"), QColor(0x88, 0x88, 0x88));
    });
    connect(m_sam, &SamBackend::errorOccurred, this, [this](const QString &message) {
        setSamReady(false);
        m_controlBar->setSamStatus(tr("error"), QColor(0xc0, 0x39, 0x2b));
        if (!m_samErrorShown) {
            m_samErrorShown = true;
            QMessageBox::critical(
                this, tr("SAM Backend Error"),
                tr("The SAM backend could not start or crashed:\n\n%1\n\n"
                   "Fix the issue, then retry with SAM -> Start Backend.").arg(message));
        }
    });

    // Core functionality: bring the backend up and load the model automatically.
    m_controlBar->setSamStatus(tr("starting..."), QColor(0xd0, 0x8a, 0x1a));
    startSamBackend();

    statusBar()->showMessage(tr("Ready. Open an image folder to begin."));
}

void MainWindow::buildActions()
{
    // Navigation.
    m_prevAction = new QAction(tr("Prev"), this);
    m_prevAction->setShortcut(QKeySequence(Qt::Key_Left));
    connect(m_prevAction, &QAction::triggered, this, &MainWindow::previousImage);

    m_nextAction = new QAction(tr("Next"), this);
    m_nextAction->setShortcut(QKeySequence(Qt::Key_Right));
    connect(m_nextAction, &QAction::triggered, this, &MainWindow::nextImage);

    // Drawing modes (exclusive toggle).
    auto *modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);

    m_rectModeAction = new QAction(tr("Rectangle"), this);
    m_rectModeAction->setCheckable(true);
    m_rectModeAction->setChecked(true);
    m_rectModeAction->setShortcut(QKeySequence(tr("R")));
    modeGroup->addAction(m_rectModeAction);
    connect(m_rectModeAction, &QAction::triggered, this, [this] {
        m_view->setMode(ImageView::Mode::Rectangle);
        statusBar()->showMessage(tr("Rectangle mode: click two opposite corners."), 4000);
    });

    m_polyModeAction = new QAction(tr("Polygon"), this);
    m_polyModeAction->setCheckable(true);
    m_polyModeAction->setShortcut(QKeySequence(tr("P")));
    modeGroup->addAction(m_polyModeAction);
    connect(m_polyModeAction, &QAction::triggered, this, [this] {
        m_view->setMode(ImageView::Mode::Polygon);
        statusBar()->showMessage(
            tr("Polygon mode: click vertices; click near the first to close."), 4000);
    });

    // Accept the in-progress / pending shape (also bound to Enter on the canvas).
    m_acceptAction = new QAction(tr("Accept (Enter)"), this);
    connect(m_acceptAction, &QAction::triggered, m_view, &ImageView::acceptPendingShape);

    // SAM video tracking (disabled until the model is ready).
    m_loadMemoryAction = new QAction(tr("Load to Memory"), this);
    m_loadMemoryAction->setShortcut(QKeySequence(tr("M")));
    connect(m_loadMemoryAction, &QAction::triggered, this, &MainWindow::loadPolygonToMemory);

    m_predictAction = new QAction(tr("Predict"), this);
    m_predictAction->setShortcut(QKeySequence(Qt::Key_Space));
    connect(m_predictAction, &QAction::triggered, this, &MainWindow::predictThisFrame);

    m_resetTrackAction = new QAction(tr("Reset Tracking"), this);
    connect(m_resetTrackAction, &QAction::triggered, this, &MainWindow::resetTracking);

    // Register every action on the window so its shortcut works even when the
    // action lives only on the bottom toolbar, not in a menu.
    for (QAction *a : { m_prevAction, m_nextAction, m_rectModeAction, m_polyModeAction,
                        m_acceptAction, m_loadMemoryAction, m_predictAction,
                        m_resetTrackAction }) {
        addAction(a);
    }

    setSamReady(false);  // SAM actions off until the model loads
}

void MainWindow::setSamReady(bool ready)
{
    m_samReady = ready;
    m_loadMemoryAction->setEnabled(ready);
    m_predictAction->setEnabled(ready);
    m_resetTrackAction->setEnabled(ready);
}

void MainWindow::buildCentralWidget()
{
    ControlBar::Actions a;
    a.prev          = m_prevAction;
    a.next          = m_nextAction;
    a.rectMode      = m_rectModeAction;
    a.polyMode      = m_polyModeAction;
    a.accept        = m_acceptAction;
    a.loadMemory    = m_loadMemoryAction;
    a.predict       = m_predictAction;
    a.resetTracking = m_resetTrackAction;
    m_controlBar = new ControlBar(a);

    auto *container = new QWidget;
    auto *layout    = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view, /*stretch=*/1);
    layout->addWidget(m_controlBar);
    setCentralWidget(container);
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

    // --- SAM (model choice only) ---------------------------------------
    // The backend and model load automatically at startup; tracking controls
    // live on the bottom ControlBar. The SAM menu just picks WHICH model to use.
    auto *samMenu = menuBar()->addMenu(tr("SA&M"));
    auto *modelGroup = new QActionGroup(this);
    modelGroup->setExclusive(true);
    for (int i = 0; i < kSamModelCount; ++i) {
        auto *modelAction = samMenu->addAction(tr(kSamModels[i].name));
        modelAction->setCheckable(true);
        modelAction->setChecked(i == m_modelIndex);
        modelGroup->addAction(modelAction);
        connect(modelAction, &QAction::triggered, this, [this, i] { selectModel(i); });
    }

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
    m_samErrorShown = false;
    m_controlBar->setSamStatus(tr("starting..."), QColor(0xd0, 0x8a, 0x1a));
    statusBar()->showMessage(tr("Starting SAM backend..."), 3000);
    m_sam->start();
}

void MainWindow::loadSamModel()
{
    if (!m_sam->isRunning()) {
        return;  // load is auto-triggered once the backend is up
    }

    // Resolve the submodule relative to the repo root (the .exe lives in
    // build/bin); the checkpoint/config come from the selected model variant.
    QDir repo(QApplication::applicationDirPath());
    repo.cdUp();   // build/bin -> build
    repo.cdUp();   // build     -> repo root

    QSettings settings;
    const QString sam2Root = settings.value(QStringLiteral("sam2Root"),
        repo.absoluteFilePath(QStringLiteral("third_party/sam2"))).toString();
    const QString device = settings.value(QStringLiteral("samDevice"),
        QStringLiteral("cpu")).toString();

    const SamModel &model = kSamModels[m_modelIndex];
    const QString checkpoint =
        repo.absoluteFilePath(QStringLiteral("models/") + QLatin1String(model.checkpoint));

    if (!QFileInfo::exists(checkpoint)) {
        setSamReady(false);
        m_controlBar->setSamStatus(tr("error"), QColor(0xc0, 0x39, 0x2b));
        QMessageBox::warning(
            this, tr("Model Not Found"),
            tr("The %1 model checkpoint was not found:\n\n%2\n\n"
               "Download it into the models/ folder (see models/README.md), "
               "or pick another model under SAM -> Model.")
                .arg(QLatin1String(model.name), checkpoint));
        return;
    }

    m_controlBar->setSamStatus(tr("loading model..."), QColor(0xd0, 0x8a, 0x1a));
    statusBar()->showMessage(tr("Loading SAM2 %1 model...").arg(QLatin1String(model.name)));
    m_sam->loadModel(checkpoint, QLatin1String(model.config), sam2Root, device);
}

void MainWindow::selectModel(int index)
{
    if (index < 0 || index >= kSamModelCount) {
        return;
    }
    m_modelIndex = index;
    QSettings().setValue(QStringLiteral("samModelIndex"), index);
    if (m_sam->isRunning()) {
        setSamReady(false);   // disable tracking until the new model is ready
        loadSamModel();       // reload with the chosen variant
    }
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

    if (command == QStringLiteral("load_model")) {
        if (status == QStringLiteral("ok")) {
            setSamReady(true);
            m_controlBar->setSamStatus(tr("ready"), QColor(0x2e, 0x8b, 0x57));
            statusBar()->showMessage(tr("SAM2 model loaded - ready."), 4000);
        } else {
            setSamReady(false);
            m_controlBar->setSamStatus(tr("error"), QColor(0xc0, 0x39, 0x2b));
            QMessageBox::warning(
                this, tr("SAM Model Failed to Load"),
                tr("The SAM2 model could not be loaded:\n\n%1\n\n"
                   "Check the checkpoint path, then retry with SAM -> Load Model.").arg(message));
        }
        return;
    }

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

    // Any other reply: surface it on the status bar.
    statusBar()->showMessage(tr("SAM: %1 (%2)").arg(status, message), 5000);
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
    if (m_controlBar) {
        m_controlBar->setFrameCounter(
            hasImages ? tr("%1 / %2").arg(m_currentIndex + 1).arg(m_imagePaths.size())
                      : tr("- / -"));
    }
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
