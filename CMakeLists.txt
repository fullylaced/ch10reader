cmake_minimum_required(VERSION 3.15)
project(ch10reader C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_GUI "Build the wxWidgets GUI (ch10gui)" ON)
option(ENABLE_VIDEO_PLAYBACK "Decode video channels with FFmpeg for in-app playback" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------------------
# ch10core: all parsing/decoding. Both front ends link this.
# ---------------------------------------------------------------------------
add_library(ch10core STATIC
    src/packet.cpp
    src/reader.cpp
    src/tmats.cpp
    src/time_f1.cpp
    src/ms1553_f1.cpp
    src/video.cpp
    src/session.cpp
    src/video_stream.cpp
    src/annotation.cpp
    src/writer.cpp
    src/ops.cpp
    src/audio.cpp
    src/ethernet.cpp
    src/message_f0.cpp
    src/wordmap.cpp
    src/userdata.cpp
)
target_include_directories(ch10core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# --- optional FFmpeg (libavformat/libavcodec/libswscale) for video playback.
#     Windows: the vcpkg manifest builds it and find_package(FFMPEG) finds it.
#     Linux:   sudo apt install libavformat-dev libavcodec-dev libswscale-dev
if(ENABLE_VIDEO_PLAYBACK)
    set(CH10_FFMPEG_FOUND FALSE)
    find_package(FFMPEG QUIET)                       # vcpkg's wrapper
    if(FFMPEG_FOUND)
        target_include_directories(ch10core PUBLIC ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(ch10core PUBLIC ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(ch10core PUBLIC ${FFMPEG_LIBRARIES})
        set(CH10_FFMPEG_FOUND TRUE)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(AV QUIET IMPORTED_TARGET
                              libavformat libavcodec libswscale libavutil)
            if(AV_FOUND)
                target_link_libraries(ch10core PUBLIC PkgConfig::AV)
                set(CH10_FFMPEG_FOUND TRUE)
            endif()
        endif()
    endif()
    if(CH10_FFMPEG_FOUND)
        target_compile_definitions(ch10core PUBLIC CH10_HAVE_FFMPEG)
        message(STATUS "FFmpeg found - video playback enabled")
    else()
        message(WARNING "FFmpeg not found - building without video playback "
                        "(extraction to .ts still works)")
    endif()
endif()

if(MSVC)
    target_compile_options(ch10core PRIVATE /W4)
    target_compile_definitions(ch10core PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ch10core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# ch10dump: command-line tool
# ---------------------------------------------------------------------------
add_executable(ch10dump src/main.cpp)
target_link_libraries(ch10dump PRIVATE ch10core)
if(MSVC)
    target_compile_options(ch10dump PRIVATE /W4)
else()
    target_compile_options(ch10dump PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# ch10gui: wxWidgets desktop application
#   Windows: wxWidgets comes from vcpkg (see vcpkg.json / CMakePresets.json)
#   Linux:   sudo apt install libwxgtk3.2-dev
# ---------------------------------------------------------------------------
if(BUILD_GUI)
    find_package(wxWidgets REQUIRED COMPONENTS aui core base)
    include(${wxWidgets_USE_FILE})

    set(GUI_SOURCES
        gui/app.cpp
        gui/main_frame.cpp   gui/main_frame.h
        gui/playback.cpp     gui/playback.h
        gui/timeline_panel.cpp gui/timeline_panel.h
        gui/video_panel.cpp  gui/video_panel.h
        gui/packets_panel.cpp gui/packets_panel.h
        gui/ms1553_panel.cpp gui/ms1553_panel.h
        gui/annotations_panel.cpp gui/annotations_panel.h
        gui/tool_dialogs.cpp gui/tool_dialogs.h
        gui/theme.cpp        gui/theme.h
        gui/audio_engine.cpp gui/audio_engine.h
        gui/audio_panel.cpp  gui/audio_panel.h
        gui/ethernet_panel.cpp gui/ethernet_panel.h
        third_party/miniaudio_impl.c
    )
    if(WIN32)
        list(APPEND GUI_SOURCES gui/app.rc)   # manifest for themed controls / DPI
    endif()

    # WIN32 => no console window on Windows; ignored elsewhere.
    add_executable(ch10gui WIN32 ${GUI_SOURCES})
    target_link_libraries(ch10gui PRIVATE ch10core ${wxWidgets_LIBRARIES})
    target_include_directories(ch10gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gui
                                               ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    if(UNIX AND NOT APPLE)
        target_link_libraries(ch10gui PRIVATE ${CMAKE_DL_LIBS} pthread m)   # miniaudio loads ALSA/Pulse at runtime
    endif()
    # miniaudio is third-party C: keep its warnings out of our -Wall/W4 build
    set_source_files_properties(third_party/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/w,-w>")
    if(MSVC)
        target_compile_options(ch10gui PRIVATE /W4)
    else()
        target_compile_options(ch10gui PRIVATE -Wall -Wextra)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests: end-to-end checks of every CLI operation against a synthetic file
#   cmake --build build && ctest --test-dir build --output-on-failure
# ---------------------------------------------------------------------------
option(BUILD_TESTING "Register the ch10dump end-to-end tests with CTest" ON)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        enable_testing()
        add_test(NAME ch10dump_end_to_end
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.py
                         $<TARGET_FILE:ch10dump> ${CMAKE_CURRENT_SOURCE_DIR}
                         ${CMAKE_CURRENT_BINARY_DIR}/test_work)
    else()
        message(STATUS "Python 3 not found - tests not registered")
    endif()
endif()
