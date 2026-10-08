function(fb_add_test name)
    add_executable(test_${name} test_${name}.c ${ARGN})
    target_include_directories(test_${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_${name} PRIVATE fb_warnings)
    if (MSVC)
        target_compile_definitions(test_${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(test_${name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(test_${name} PRIVATE -UNDEBUG)
        target_link_libraries(test_${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

add_executable(meeting_tool_fixture meeting_tool_fixture.c)
target_include_directories(meeting_tool_fixture PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(Python3 QUIET COMPONENTS Interpreter)
if (Python3_FOUND)
    add_test(NAME meeting_pipeline COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_meeting_pipeline.py
        $<TARGET_FILE:fbdb> $<TARGET_FILE:meeting_tool_fixture>)
endif()

fb_add_test(ring_buffer ${CMAKE_SOURCE_DIR}/src/audio/ring_buffer.c)
fb_add_test(wav_writer ${CMAKE_SOURCE_DIR}/src/audio/wav_writer.c)
fb_add_test(wav_stream ${CMAKE_SOURCE_DIR}/src/audio/wav_stream.c)
fb_add_test(notepad ${CMAKE_SOURCE_DIR}/src/tui/widget_notepad.c
    ${CMAKE_SOURCE_DIR}/src/tui/layout.c ${CMAKE_SOURCE_DIR}/src/util/utf8.c)
fb_add_test(meeting_document ${CMAKE_SOURCE_DIR}/src/meeting/meeting_document.c)
target_link_libraries(test_meeting_document PRIVATE cjson)

# calibr_results/calibr_log_tail render into an FbTextView, whose render
# path (unused by this test, but still linked -- object files link as a
# whole) touches tui/layout.c and, transitively, the screen backend; harmless
# to link since fb_screen_init is never called here.
if (WIN32)
    set(fb_test_platform_sources
        ${CMAKE_SOURCE_DIR}/src/platform/win/console_win.c
        ${CMAKE_SOURCE_DIR}/src/platform/win/paths_win.c
        ${CMAKE_SOURCE_DIR}/src/platform/win/dirlist_win.c
    )
else()
    set(fb_test_platform_sources
        ${CMAKE_SOURCE_DIR}/src/tui/backend_termbox2.c
        ${CMAKE_SOURCE_DIR}/src/platform/posix/paths_posix.c
        ${CMAKE_SOURCE_DIR}/src/platform/posix/dirlist_posix.c
    )
endif()

get_target_property(fb_meeting_test_sources fbdb SOURCES)
list(FILTER fb_meeting_test_sources EXCLUDE REGEX "src/main.c|console_win.c|backend_termbox2.c")
list(TRANSFORM fb_meeting_test_sources PREPEND "${CMAKE_SOURCE_DIR}/")
fb_add_test(meeting_ui ${fb_meeting_test_sources})
get_target_property(fb_meeting_definitions fbdb COMPILE_DEFINITIONS)
target_compile_definitions(test_meeting_ui PRIVATE ${fb_meeting_definitions})
target_link_libraries(test_meeting_ui PRIVATE miniaudio cjson)
add_test(NAME meeting_lifecycle COMMAND test_meeting_ui $<TARGET_FILE:meeting_tool_fixture>)
if (WIN32)
    target_link_libraries(test_meeting_ui PRIVATE ws2_32 ole32 shell32)
else()
    target_link_libraries(test_meeting_ui PRIVATE pthread)
endif()

fb_add_test(calibr_results
    ${CMAKE_SOURCE_DIR}/src/calibr/calibr_results.c
    ${CMAKE_SOURCE_DIR}/src/calibr/calibr_log_tail.c
    ${CMAKE_SOURCE_DIR}/src/tui/widget_textview.c
    ${CMAKE_SOURCE_DIR}/src/tui/layout.c
    ${CMAKE_SOURCE_DIR}/src/util/fileutil.c
    ${CMAKE_SOURCE_DIR}/src/util/utf8.c
    ${fb_test_platform_sources}
)
target_link_libraries(test_calibr_results PRIVATE cjson)
if (WIN32)
    target_link_libraries(test_calibr_results PRIVATE ws2_32 ole32)
else()
    target_link_libraries(test_calibr_results PRIVATE termbox2)
endif()
