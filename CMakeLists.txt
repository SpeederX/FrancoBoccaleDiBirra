cmake_minimum_required(VERSION 3.21)
project(FrancoBoccaleDiBirra C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/Warnings.cmake)

# --- cJSON (small static library) -------------------------------------------
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

# --- miniaudio (single header, kept minimal) --------------------------------
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio INTERFACE third_party/miniaudio)
target_compile_definitions(miniaudio INTERFACE
    MA_NO_DECODING
    MA_NO_ENCODING
    MA_NO_ENGINE
    MA_NO_NODE_GRAPH
    MA_NO_GENERATION
    MA_NO_RESOURCE_MANAGER
)
if (WIN32)
    target_compile_definitions(miniaudio INTERFACE
        MA_ENABLE_ONLY_SPECIFIC_BACKENDS
        MA_ENABLE_WASAPI
    )
endif()

# --- termbox2 (single header, POSIX only) -----------------------------------
add_library(termbox2 INTERFACE)
target_include_directories(termbox2 INTERFACE third_party/termbox2)

# --- main executable ---------------------------------------------------------
add_executable(fbdb
    src/main.c
    src/app/app.c
    src/app/command_dispatch.c
    src/app/sweep.c
    src/tui/layout.c
    src/tui/widget_cmdline.c
    src/tui/fx_sphere.c
    src/tui/fx_waveform.c
    src/tui/fx_pencil.c
    src/tui/fx_hypercube.c
    src/tui/fx_tetris.c
    src/tui/widget_file_picker.c
    src/tui/widget_textview.c
    src/calibr/calibr_paths.c
    src/calibr/calibr_results.c
    src/calibr/calibr_log_tail.c
    src/audio/ring_buffer.c
    src/audio/mic_capture.c
    src/audio/wav_writer.c
    src/net/http_client.c
    src/net/llama_client.c
    src/backend/backend_mgr.c
    src/config/config.c
    src/transcribe/transcribe.c
    src/intent/intent_brain.c
    src/chat/chat_job.c
    src/diary/diary_job.c
    src/diary/diary.c
    src/project/project.c
    src/util/fileutil.c
    src/util/utf8.c
)
target_link_libraries(fbdb PRIVATE miniaudio)
target_include_directories(fbdb PRIVATE src)
target_link_libraries(fbdb PRIVATE fb_warnings cjson)
if (MSVC)
    # This is a portable-C codebase that also targets POSIX toolchains, so it
    # intentionally uses standard CRT functions (snprintf, strncpy, ...)
    # rather than MSVC's non-standard *_s variants.
    target_compile_definitions(fbdb PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if (WIN32)
    target_sources(fbdb PRIVATE
        src/platform/win/console_win.c
        src/platform/win/paths_win.c
        src/platform/win/sock_win.c
        src/platform/win/proc_win.c
        src/platform/win/dirlist_win.c
        src/platform/win/thread_win.c
        src/platform/win/open_url_win.c
    )
    target_link_libraries(fbdb PRIVATE ws2_32 ole32 shell32)
    target_compile_definitions(fbdb PRIVATE
        FB_LLAMA_SERVER_DEFAULT_NAME="llama-server.exe"
        FB_SHERPA_ONNX_DEFAULT_NAME="sherpa-onnx-offline.exe"
    )
else()
    target_sources(fbdb PRIVATE
        src/tui/backend_termbox2.c
        src/platform/posix/paths_posix.c
        src/platform/posix/sock_posix.c
        src/platform/posix/proc_posix.c
        src/platform/posix/dirlist_posix.c
        src/platform/posix/thread_posix.c
        src/platform/posix/open_url_posix.c
    )
    target_link_libraries(fbdb PRIVATE termbox2 pthread m)
    target_compile_definitions(fbdb PRIVATE
        FB_LLAMA_SERVER_DEFAULT_NAME="llama-server"
        FB_SHERPA_ONNX_DEFAULT_NAME="sherpa-onnx-offline"
    )
endif()

enable_testing()
add_subdirectory(tests)
