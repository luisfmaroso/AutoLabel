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
    backend/SamBackend.h
    backend/SamBackend.cpp
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

# Copy the Python SAM service next to the executable so SamBackend can launch it
# via applicationDirPath() (same "ship it next to the .exe" convention
# InferenceVisualizer uses for its model/DLL).
add_custom_command(TARGET AutoLabel POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/python/sam_service.py"
        "$<TARGET_FILE_DIR:AutoLabel>"
    COMMENT "Copying sam_service.py next to AutoLabel.exe")
