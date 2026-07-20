qt_add_executable(hopline
    main.cpp
    app/MainWindow.cpp
    app/MainWindow.h
    app/PreviewWidget.cpp
    app/PreviewWidget.h
    engine/AudioOutput.cpp
    engine/AudioOutput.h
    engine/Clock.h
    engine/FrameQueue.cpp
    engine/FrameQueue.h
    engine/Player.cpp
    engine/Player.h
    engine/RingBuffer.cpp
    engine/RingBuffer.h
    media/AudioDecoder.cpp
    media/AudioDecoder.h
    media/MediaProbe.cpp
    media/MediaProbe.h
    media/VideoDecoder.cpp
    media/VideoDecoder.h
    media/VideoFrame.h
    model/Clip.h
    model/Command.cpp
    model/Command.h
    model/Commands.cpp
    model/Commands.h
    model/Media.h
    model/Project.cpp
    model/Project.h
    model/Sequence.cpp
    model/Sequence.h
    model/Time.h
    model/Track.cpp
    model/Track.h
)

find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h" REQUIRED)

target_include_directories(hopline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${MINIAUDIO_INCLUDE_DIRS})
target_link_libraries(hopline PRIVATE Qt6::Widgets PkgConfig::FFMPEG)

set_target_properties(hopline PROPERTIES WIN32_EXECUTABLE ON)

# Qt DLLs and the platform plugin aren't copied by vcpkg's applocal deployment.
if(WIN32)
    find_program(WINDEPLOYQT windeployqt HINTS $<TARGET_FILE_DIR:Qt6::qmake>)
    add_custom_command(TARGET hopline POST_BUILD
        COMMAND ${WINDEPLOYQT} --no-translations --no-system-d3d-compiler
                $<IF:$<CONFIG:Debug>,--debug,--release>
                $<TARGET_FILE:hopline>
        VERBATIM)
endif()

if(MSVC)
    target_compile_options(hopline PRIVATE /W4 /permissive-)
else()
    target_compile_options(hopline PRIVATE -Wall -Wextra)
endif()
