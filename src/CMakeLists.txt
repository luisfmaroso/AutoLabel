qt_add_executable(AutoLabel
    WIN32
    main.cpp
)

target_sources(AutoLabel PRIVATE
    app/MainWindow.h
    app/MainWindow.cpp
    ui/ImageView.h
    ui/ImageView.cpp
    ui/SettingsDialog.h
    ui/SettingsDialog.cpp
    annotation/Shape.h
    annotation/AnnotationModel.h
    annotation/AnnotationModel.cpp
    annotation/ClassColors.h
    annotation/ClassColors.cpp
    annotation/YoloIo.h
    annotation/YoloIo.cpp
)

target_include_directories(AutoLabel PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(AutoLabel PRIVATE
    Qt6::Widgets
)

if(MSVC)
    target_compile_options(AutoLabel PRIVATE /W4 /permissive-)
else()
    target_compile_options(AutoLabel PRIVATE -Wall -Wextra -Wpedantic)
endif()
