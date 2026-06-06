#pragma once

#include "annotation/ClassColors.h"
#include "annotation/Shape.h"

#include <QMainWindow>
#include <QSize>
#include <QStringList>

class AnnotationModel;
class ImageView;
class SamBackend;
class QAction;
class QJsonObject;

// MainWindow: top-level window and application glue. It owns the list of images
// in the open folder, the annotation model, and drives navigation, class entry,
// and YOLO export. Rendering and drawing interaction live in ImageView; shape
// storage lives in AnnotationModel. MainWindow wires the two together and is the
// only place that touches the filesystem for labels.
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void openImageFolder();
    void nextImage();
    void previousImage();
    void onShapeDrawn(const Shape &shape);   // assign class, commit, export
    void onRemoveLastRequested();            // delete last shape, re-export
    void openSettings();                     // edit per-class colours
    void startSamBackend();
    void pingSamBackend();
    void loadSamModel();
    void generateMask();
    void loadPolygonToMemory();   // M: register current frame's polygon as memory
    void predictThisFrame();      // Space: predict current frame from memory
    void resetTracking();
    void onSamResponse(const QJsonObject &reply);
    void showAbout();

private:
    void buildCentralWidget();
    void buildMenus();
    void loadCurrentImage();
    void updateNavigationActions();
    void saveAnnotations() const;            // (re)write the current image's .txt
    QString labelPathForCurrentImage() const;
    QString labelPathForIndex(int index) const;

    // UI
    ImageView       *m_view        = nullptr;
    AnnotationModel *m_annotations = nullptr;
    SamBackend      *m_sam         = nullptr;
    ClassColors      m_classColors;          // per-class render colours (persisted)

    QAction *m_prevAction = nullptr;
    QAction *m_nextAction = nullptr;
    QAction *m_pingSamAction = nullptr;

    // Folder state.
    QStringList m_imagePaths;
    int         m_currentIndex = -1;
    QSize       m_currentImageSize;          // size of the image on screen (for export)

    int m_lastClassId = 0;                   // remembered as the class dialog default

    // Video tracking state.
    bool m_memorySeeded   = false;   // a polygon has been loaded into SAM memory
    int  m_trackClassId   = 0;       // class to apply to predicted polygons
};
