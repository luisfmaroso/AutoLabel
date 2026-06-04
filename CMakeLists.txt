cmake_minimum_required(VERSION 3.21)

project(AutoLabel
    VERSION 0.1.0
    DESCRIPTION "Qt + OpenCV + SAM2 image annotation tool for YOLO segmentation datasets (learning project)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put binaries in build/bin so we don't dig through CMake's generator-specific
# subdirectories to find the .exe. (Same convention as InferenceVisualizer.)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- Qt auto-detection -------------------------------------------------------
# The Qt Installer puts Qt under ~/Qt on Linux/macOS and C:/Qt on Windows.
# We glob for the newest 6.x.x subdir so the version doesn't need to be
# hardcoded. Override the root with -DQT_ROOT=... or the compiler subdir with
# -DQT_COMPILER_SUBDIR=... if your layout differs (e.g. mingw_64 on Windows).
if(WIN32)
    set(QT_ROOT "C:/Qt" CACHE PATH "Root Qt Installer directory")
    set(QT_COMPILER_SUBDIR "mingw_64" CACHE STRING "Qt compiler subdirectory")
else()
    set(QT_ROOT "$ENV{HOME}/Qt" CACHE PATH "Root Qt Installer directory")
    set(QT_COMPILER_SUBDIR "gcc_64" CACHE STRING "Qt compiler subdirectory")
endif()

file(GLOB _qt_versions LIST_DIRECTORIES true "${QT_ROOT}/6.*")
list(SORT _qt_versions COMPARE NATURAL ORDER DESCENDING)
if(_qt_versions)
    list(GET _qt_versions 0 _qt_latest)
    list(APPEND CMAKE_PREFIX_PATH "${_qt_latest}/${QT_COMPILER_SUBDIR}")
    message(STATUS "Qt auto-detected: ${_qt_latest}/${QT_COMPILER_SUBDIR}")
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

# --- OpenCV ------------------------------------------------------------------
# The C++ frontend deliberately does NOT link OpenCV. Image loading/display use
# Qt (QImageReader + QGraphicsView). All OpenCV work -- mask post-processing and
# contour-to-polygon extraction (Phase 8) -- lives in the Python SAM2 backend,
# where the cv2 package is already available. This keeps the C++ build free of a
# heavy native dependency and matches the frontend=Qt / backend=Python split.

add_subdirectory(src)
