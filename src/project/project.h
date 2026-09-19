#ifndef FB_PROJECT_H
#define FB_PROJECT_H

/*
 * Project mode: the fourth input mode, folder-scoped context for working
 * on a specific codebase/project rather than a free-floating conversation
 * (see diary/diary.h and chat/chat_job.h for the other two live-chat
 * modes). Deliberately an empty box right now -- app.c wires it up as a
 * selectable mode (Tab-cycles into it, gets its own color/effect/mode tag)
 * so it has a real place to grow into, but fb_project_init/_tick/_shutdown
 * below do nothing yet. This file, plus project.c, is the intended home
 * for that logic -- built out in its own worktree (see the repo's
 * `feature/project-mode` branch) precisely so it doesn't collide with
 * concurrent diary/memory work, which lives entirely in diary/*, chat/*,
 * and app/sweep.c and never touches this directory.
 *
 * Candidate scope, not yet decided or built (see the project's backlog):
 * a Graphify-backed knowledge graph of the working directory for
 * structural context (nodes/edges over code symbols -- see the session
 * notes on github.com/Graphify-Labs/graphify), Prime-Agent-style focused
 * single-workflow execution, and/or a lighter directory-scoped file/tool
 * access model of its own. None of that is implemented here -- this is
 * just the seam.
 */

typedef struct {
    int placeholder; /* nothing here yet -- keeps the struct non-empty for C89/MSVC */
} FbProject;

void fb_project_init(FbProject *p);
void fb_project_shutdown(FbProject *p);

#endif /* FB_PROJECT_H */
