#include "intent/intent_brain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "intent/intent_schema.h"
#include "net/http_client.h"

static int eq_ci(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/* The schema declares "model" as ["string", "null"], but confirmed
   directly against a real classify response (input "load", nothing else
   specified) that this ~1B model doesn't reliably emit a real JSON null
   for an unset field -- it emits the literal string "None" instead
   (Python's null spelling, not JSON's), for every nullable field at once.
   Treating that string as an actual model name sent a real lookup for a
   model called "None" instead of falling through to /load's own "exactly
   one registered -- use it" default. Normalize the handful of placeholder
   spellings a small model might use to "no value", same as a real null. */
static int is_null_placeholder(const char *s) {
    return s[0] == '\0' || eq_ci(s, "none") || eq_ci(s, "null") || eq_ci(s, "n/a") ||
           eq_ci(s, "unspecified") || eq_ci(s, "unknown");
}

static void set_arg1(FbCommand *cmd, const cJSON *value) {
    if (cJSON_IsString(value) && !is_null_placeholder(value->valuestring)) {
        snprintf(cmd->arg1, sizeof(cmd->arg1), "%s", value->valuestring);
    }
}

int fb_intent_brain_classify(int port, const char *text, FbCommand *out_cmd, char *out_raw,
                              size_t out_raw_cap) {
    memset(out_cmd, 0, sizeof(*out_cmd));
    out_cmd->kind = FB_CMD_UNKNOWN;
    if (out_raw && out_raw_cap > 0) out_raw[0] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "qwen");
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", FB_INTENT_SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, sys_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", text);
    cJSON_AddItemToArray(messages, user_msg);

    cJSON *response_format = cJSON_Parse(FB_INTENT_RESPONSE_FORMAT_JSON);
    cJSON_AddItemToObject(root, "response_format", response_format);
    cJSON_AddNumberToObject(root, "temperature", 0.1);
    cJSON_AddNumberToObject(root, "max_tokens", 150);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;

    FbHttpResponse resp;
    int rc = fb_http_request(port, "POST", "/v1/chat/completions", "application/json", body,
                              strlen(body), 5000, &resp);
    cJSON_free(body);
    if (rc != 0) return -1;

    cJSON *outer = cJSON_Parse(resp.body);
    fb_http_response_free(&resp);
    if (!outer) return 0; /* stays FB_CMD_UNKNOWN */

    cJSON *choices = cJSON_GetObjectItemCaseSensitive(outer, "choices");
    cJSON *choice0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *message = choice0 ? cJSON_GetObjectItemCaseSensitive(choice0, "message") : NULL;
    cJSON *content = message ? cJSON_GetObjectItemCaseSensitive(message, "content") : NULL;

    if (!cJSON_IsString(content) || content->valuestring[0] == '\0') {
        cJSON_Delete(outer);
        return 0; /* stays FB_CMD_UNKNOWN */
    }

    if (out_raw && out_raw_cap > 0) snprintf(out_raw, out_raw_cap, "%s", content->valuestring);

    cJSON *inner = cJSON_Parse(content->valuestring); /* copies the string; safe before deleting outer */
    cJSON_Delete(outer);
    if (!inner) return 0; /* model didn't actually return valid JSON: stays UNKNOWN */

    cJSON *intent = cJSON_GetObjectItemCaseSensitive(inner, "intent");
    cJSON *model = cJSON_GetObjectItemCaseSensitive(inner, "model");
    cJSON *lang = cJSON_GetObjectItemCaseSensitive(inner, "lang");
    const char *intent_str = cJSON_IsString(intent) ? intent->valuestring : "unknown";

    if (strcmp(intent_str, "load_model") == 0) {
        out_cmd->kind = FB_CMD_LOAD;
        set_arg1(out_cmd, model);
    } else if (strcmp(intent_str, "unload_model") == 0) {
        out_cmd->kind = FB_CMD_UNLOAD;
        set_arg1(out_cmd, model);
    } else if (strcmp(intent_str, "add_model") == 0) {
        out_cmd->kind = FB_CMD_ADD;
        set_arg1(out_cmd, model); /* arg2 (path) stays empty on purpose -- see header comment */
    } else if (strcmp(intent_str, "remove_model") == 0) {
        out_cmd->kind = FB_CMD_REMOVE;
        set_arg1(out_cmd, model);
    } else if (strcmp(intent_str, "set_lang") == 0) {
        out_cmd->kind = FB_CMD_LANG;
        set_arg1(out_cmd, lang);
    } else {
        out_cmd->kind = FB_CMD_UNKNOWN;
    }

    cJSON_Delete(inner);
    return 0;
}
