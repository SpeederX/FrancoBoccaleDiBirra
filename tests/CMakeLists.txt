function(fb_add_test name)
    add_executable(test_${name} test_${name}.c ${ARGN})
    target_include_directories(test_${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_${name} PRIVATE fb_warnings)
    if (MSVC)
        target_compile_definitions(test_${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_link_libraries(test_${name} PRIVATE m)
    endif()
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

fb_add_test(ring_buffer ${CMAKE_SOURCE_DIR}/src/audio/ring_buffer.c)
fb_add_test(wav_writer ${CMAKE_SOURCE_DIR}/src/audio/wav_writer.c)

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

fb_add_test(calibr_results
    ${CMAKE_SOURCE_DIR}/src/calibr/calibr_results.c
    ${CMAKE_SOURCE_DIR}/src/calibr/calibr_log_tail.c
    ${CMAKE_SOURCE_DIR}/src/tui/widget_textview.c
    ${CMAKE_SOURCE_DIR}/src/tui/layout.c
    ${CMAKE_SOURCE_DIR}/src/util/fileutil.c
    ${fb_test_platform_sources}
)
target_link_libraries(test_calibr_results PRIVATE cjson)
if (WIN32)
    target_link_libraries(test_calibr_results PRIVATE ws2_32 ole32)
else()
    target_link_libraries(test_calibr_results PRIVATE termbox2)
endif()
