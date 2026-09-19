#include "project/project.h"

#include <string.h>

void fb_project_init(FbProject *p) { memset(p, 0, sizeof(*p)); }

void fb_project_shutdown(FbProject *p) { (void)p; }
