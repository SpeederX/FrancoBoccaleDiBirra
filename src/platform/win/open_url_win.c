#include "platform/open_url.h"

#include <windows.h>
#include <shellapi.h>

void fb_open_in_browser(const char *path) {
    ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
}
