add_library(fb_warnings INTERFACE)

if (MSVC)
    target_compile_options(fb_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(fb_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()
