#ifndef FB_COMMAND_DISPATCH_H
#define FB_COMMAND_DISPATCH_H

#include "app/app.h"

typedef enum {
    FB_CMD_UNKNOWN = 0,
    FB_CMD_LOAD,
    FB_CMD_UNLOAD,
    FB_CMD_ADD,
    FB_CMD_REMOVE,
    FB_CMD_LANG,
    FB_CMD_DOWNLOAD, /* parsed now, reserved; not implemented until a later phase */
    FB_CMD_LIST,     /* /list or /models: registered models and their loaded state */
    FB_CMD_NEW_PAGE, /* diary mode only: manually close the live page and start a fresh one */
    FB_CMD_LEADERBOARD,
    FB_CMD_LOGS,
    FB_CMD_DASHBOARD,
    FB_CMD_HELP,
    FB_CMD_QUIT
} FbCommandKind;

#define FB_CMD_ARG_MAX 512

typedef struct {
    FbCommandKind kind;
    char arg1[FB_CMD_ARG_MAX]; /* meaning depends on kind: model/lang name */
    char arg2[FB_CMD_ARG_MAX]; /* e.g. /add's path */
} FbCommand;

/* Parses a typed line like "/load name" or "/add name path". A line with
   no leading '/' or an unrecognized verb yields FB_CMD_UNKNOWN rather than
   failing — the caller decides what to do with unrecognized input (in
   milestone 7, that's where free text gets handed to the Qwen intent
   brain instead). */
FbCommand fb_command_parse(const char *line);

/* Executes a command against app's state (backend manager, model
   registry), writing a short human-readable result into
   app->last_submitted. This is the single execution path for both typed
   commands and, from milestone 7, Qwen-derived structured intents, so
   both interaction styles behave identically. */
void fb_command_execute(FbApp *app, const FbCommand *cmd);

/* Tab-completion for a partially-typed "/load <name" or "/unload <name":
   extends buf's model-name argument in place to the longest unambiguous
   completion (candidates are the model registry for /load, currently-
   loaded backends for /unload), and appends a trailing space on an exact
   single match. *len is updated to match. No-op for any other input. */
void fb_command_autocomplete(FbApp *app, char *buf, size_t *len, size_t cap);

#endif /* FB_COMMAND_DISPATCH_H */
