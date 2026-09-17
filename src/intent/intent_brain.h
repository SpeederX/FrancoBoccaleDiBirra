#ifndef FB_INTENT_BRAIN_H
#define FB_INTENT_BRAIN_H

#include "app/command_dispatch.h"

/* Reserved backend name for the resident Qwen intent brain, so app.c can
   find it in the backend manager without a dedicated field. */
#define FB_QWEN_BRAIN_NAME "qwen-intent-brain"

/*
 * Sends free text (typed without a leading '/', or transcribed) to the
 * resident Qwen model at 127.0.0.1:port and parses its structured-output
 * response into an FbCommand — the same type/execution path as a typed
 * "/" command, so a misclassified intent gets exactly the same safety
 * properties (e.g. /unload and /remove only ever act on an exact
 * registered-name match, regardless of how the command was produced).
 *
 * Blocking: a warmed-up small (~1B) model responds in roughly a second
 * (confirmed empirically), which is an accepted latency for first mile —
 * this is not polled/asynchronous like transcription's multi-second model
 * load.
 *
 * add_model's arg2 (path) is deliberately never filled from the model's
 * output even if it guesses one — an LLM-hallucinated filesystem path
 * can't be trusted, so add_model always routes through the real file
 * picker (see command_dispatch.c's FB_CMD_ADD handling for an empty path).
 *
 * Returns 0 with out_cmd filled (defaulting to FB_CMD_UNKNOWN for
 * anything that doesn't parse as expected — never left uninitialized).
 * Returns -1 only if the HTTP request itself failed (server not up,
 * timeout) — out_cmd is still safely zeroed/UNKNOWN in that case too.
 */
int fb_intent_brain_classify(int port, const char *text, FbCommand *out_cmd);

#endif /* FB_INTENT_BRAIN_H */
