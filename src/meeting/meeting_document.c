#include "meeting/meeting_document.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cJSON *field(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static int integer(const cJSON *value, int minimum) {
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           value->valuedouble >= minimum && value->valuedouble <= INT_MAX &&
           floor(value->valuedouble) == value->valuedouble;
}

static cJSON *speaker_by_id(const cJSON *document, int id) {
    cJSON *speaker;
    cJSON_ArrayForEach(speaker, field(document, "speakers")) {
        if (field(speaker, "id")->valueint == id) {
            return speaker;
        }
    }
    return NULL;
}

static int speaker_for_slot(cJSON *document, int slot) {
    if (slot < 0) {
        return 0;
    }
    cJSON *speakers = field(document, "speakers");
    cJSON *speaker;
    cJSON_ArrayForEach(speaker, speakers) {
        if (field(speaker, "slot")->valueint == slot) {
            return field(speaker, "id")->valueint;
        }
    }
    int id = cJSON_GetArraySize(speakers) + 1;
    speaker = cJSON_CreateObject();
    cJSON_AddNumberToObject(speaker, "id", id);
    cJSON_AddNumberToObject(speaker, "slot", slot);
    cJSON_AddStringToObject(speaker, "source", "numbered");
    cJSON_AddItemToArray(speakers, speaker);
    return id;
}

cJSON *fb_meeting_document_parse(const char *scene_jsonl, char *error, size_t capacity) {
    error[0] = '\0';
    cJSON *document = cJSON_CreateObject();
    cJSON_AddNumberToObject(document, "schema_version", 1);
    cJSON_AddArrayToObject(document, "speakers");
    cJSON *utterances = cJSON_AddArrayToObject(document, "utterances");
    size_t length = strlen(scene_jsonl);
    char *lines = malloc(length + 1);
    if (!lines) {
        snprintf(error, capacity, "could not allocate the transcript");
        cJSON_Delete(document);
        return NULL;
    }
    memcpy(lines, scene_jsonl, length + 1);
    char *line = lines;
    int line_number = 0;
    int updates = 0;
    while (*line) {
        line_number++;
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (*line != '\0' && strcmp(line, "\r") != 0) {
            cJSON *update = cJSON_ParseWithOpts(line, NULL, 1);
            cJSON *source = update ? field(update, "utterances") : NULL;
            if (!cJSON_IsArray(source)) {
                cJSON_Delete(update);
                snprintf(error, capacity, "invalid Parakeet JSON at line %d", line_number);
                goto failed;
            }
            updates++;
            cJSON *item;
            cJSON_ArrayForEach(item, source) {
                cJSON *slot = field(item, "speaker");
                cJSON *text = field(item, "text");
                cJSON *start = field(item, "start");
                cJSON *end = field(item, "end");
                if (!integer(slot, -1) || !cJSON_IsString(text) ||
                    !cJSON_IsNumber(start) || !cJSON_IsNumber(end) ||
                    !isfinite(start->valuedouble) || !isfinite(end->valuedouble) ||
                    start->valuedouble < 0 || end->valuedouble < start->valuedouble) {
                    cJSON_Delete(update);
                    snprintf(error, capacity, "invalid utterance at Parakeet line %d", line_number);
                    goto failed;
                }
                cJSON *utterance = cJSON_CreateObject();
                cJSON_AddNumberToObject(utterance, "id", cJSON_GetArraySize(utterances) + 1);
                cJSON_AddNumberToObject(utterance, "speaker", speaker_for_slot(document, slot->valueint));
                cJSON_AddNumberToObject(utterance, "start", start->valuedouble);
                cJSON_AddNumberToObject(utterance, "end", end->valuedouble);
                cJSON_AddStringToObject(utterance, "text", text->valuestring);
                cJSON *confidence = field(item, "conf");
                if (cJSON_IsNumber(confidence)) {
                    cJSON_AddNumberToObject(utterance, "confidence", confidence->valuedouble);
                }
                cJSON_AddItemToArray(utterances, utterance);
            }
            cJSON_Delete(update);
        }
        if (!next) {
            break;
        }
        line = next;
    }
    if (updates == 0) {
        snprintf(error, capacity, "Parakeet returned no JSON updates");
        goto failed;
    }
    free(lines);
    return document;

failed:
    free(lines);
    cJSON_Delete(document);
    return NULL;
}

static int valid_name(const char *name) {
    if (!name || !*name) {
        return 0;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 32 || *p == 127) {
            return 0;
        }
    }
    return 1;
}

int fb_meeting_document_valid(const cJSON *document) {
    cJSON *speakers = field(document, "speakers");
    cJSON *utterances = field(document, "utterances");
    if (!cJSON_IsObject(document) || !cJSON_IsArray(speakers) || !cJSON_IsArray(utterances)) {
        return 0;
    }
    int expected = 1;
    cJSON *item;
    cJSON_ArrayForEach(item, speakers) {
        cJSON *source = field(item, "source");
        cJSON *name = field(item, "name");
        if (!integer(field(item, "id"), 1) || field(item, "id")->valueint != expected++ ||
            !integer(field(item, "slot"), 0) || !cJSON_IsString(source) ||
            (strcmp(source->valuestring, "numbered") != 0 &&
             strcmp(source->valuestring, "manual") != 0 &&
             strcmp(source->valuestring, "introduction_inferred") != 0) ||
            (name && (!cJSON_IsString(name) || !valid_name(name->valuestring)))) {
            return 0;
        }
    }
    expected = 1;
    cJSON_ArrayForEach(item, utterances) {
        cJSON *speaker = field(item, "speaker");
        cJSON *start = field(item, "start");
        cJSON *end = field(item, "end");
        cJSON *cleaned = field(item, "cleaned");
        if (!integer(field(item, "id"), 1) || field(item, "id")->valueint != expected++ ||
            !integer(speaker, 0) || speaker->valueint > cJSON_GetArraySize(speakers) ||
            !cJSON_IsString(field(item, "text")) ||
            !cJSON_IsNumber(start) || !cJSON_IsNumber(end) ||
            !isfinite(start->valuedouble) || !isfinite(end->valuedouble) ||
            start->valuedouble < 0 || end->valuedouble < start->valuedouble ||
            (cleaned && !cJSON_IsString(cleaned))) {
            return 0;
        }
    }
    return 1;
}

static void set_name(cJSON *speaker, const char *name, const char *source) {
    cJSON_DeleteItemFromObjectCaseSensitive(speaker, "name");
    cJSON_AddStringToObject(speaker, "name", name);
    cJSON_ReplaceItemInObjectCaseSensitive(speaker, "source", cJSON_CreateString(source));
}

int fb_meeting_document_preserve_names(cJSON *document, const cJSON *previous) {
    cJSON *current = field(document, "utterances");
    cJSON *old = field(previous, "utterances");
    if (!fb_meeting_document_valid(previous) ||
        cJSON_GetArraySize(current) != cJSON_GetArraySize(old) ||
        cJSON_GetArraySize(field(document, "speakers")) != cJSON_GetArraySize(field(previous, "speakers"))) {
        return 0;
    }
    const char *keys[] = {"id", "speaker", "start", "end", "text"};
    for (int i = 0; i < cJSON_GetArraySize(current); ++i) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); ++k) {
            if (!cJSON_Compare(field(cJSON_GetArrayItem(current, i), keys[k]),
                               field(cJSON_GetArrayItem(old, i), keys[k]), 1)) {
                return 0;
            }
        }
    }
    cJSON *speakers = field(document, "speakers");
    for (int i = 0; i < cJSON_GetArraySize(speakers); ++i) {
        cJSON *speaker = cJSON_GetArrayItem(speakers, i);
        cJSON *prior = cJSON_GetArrayItem(field(previous, "speakers"), i);
        if (!cJSON_Compare(field(speaker, "slot"), field(prior, "slot"), 1)) {
            return 0;
        }
    }
    cJSON_ReplaceItemInObjectCaseSensitive(document, "speakers", cJSON_Duplicate(field(previous, "speakers"), 1));
    return 1;
}

int fb_meeting_document_name(cJSON *document, int id, const char *name,
                             char *error, size_t capacity) {
    cJSON *speaker = speaker_by_id(document, id);
    if (!speaker || !valid_name(name)) {
        snprintf(error, capacity, "use an existing speaker number and a nonempty name");
        return -1;
    }
    set_name(speaker, name, "manual");
    cJSON_DeleteItemFromObjectCaseSensitive(document, "report_current");
    cJSON_AddBoolToObject(document, "report_current", 0);
    cJSON_DeleteItemFromObjectCaseSensitive(speaker, "evidence");
    cJSON_DeleteItemFromObjectCaseSensitive(speaker, "utterance");
    return 0;
}

int fb_meeting_document_apply(cJSON *document, const char *response, int first, int count,
                              char *error, size_t capacity) {
    cJSON *reply = cJSON_ParseWithOpts(response, NULL, 1);
    cJSON *cleaned = reply ? field(reply, "utterances") : NULL;
    cJSON *raw = field(document, "utterances");
    if (!cJSON_IsArray(cleaned) || first < 0 || count < 1 ||
        first + count > cJSON_GetArraySize(raw) || cJSON_GetArraySize(cleaned) != count) {
        snprintf(error, capacity, "LLM output omitted utterances or returned invalid JSON");
        cJSON_Delete(reply);
        return -1;
    }
    for (int i = 0; i < count; ++i) {
        cJSON *item = cJSON_GetArrayItem(cleaned, i);
        cJSON *id = field(item, "id");
        cJSON *text = field(item, "text");
        if (!integer(id, 1) || id->valueint != first + i + 1 ||
            !cJSON_IsString(text) || !text->valuestring[0]) {
            snprintf(error, capacity, "LLM output changed an utterance ID or omitted its text");
            cJSON_Delete(reply);
            return -1;
        }
    }
    cJSON *introductions = field(reply, "introductions");
    if (introductions && !cJSON_IsArray(introductions)) {
        snprintf(error, capacity, "LLM introductions must be an array");
        cJSON_Delete(reply);
        return -1;
    }
    cJSON *intro;
    cJSON_ArrayForEach(intro, introductions) {
        cJSON *speaker_id = field(intro, "speaker");
        cJSON *utterance_id = field(intro, "utterance");
        cJSON *name = field(intro, "name");
        cJSON *quote = field(intro, "quote");
        cJSON *utterance = integer(utterance_id, 1)
                               ? cJSON_GetArrayItem(raw, utterance_id->valueint - 1) : NULL;
        if (!integer(speaker_id, 1) || !utterance ||
            utterance_id->valueint <= first || utterance_id->valueint > first + count ||
            field(utterance, "speaker")->valueint != speaker_id->valueint ||
            !cJSON_IsString(name) || !valid_name(name->valuestring) ||
            !cJSON_IsString(quote) || !quote->valuestring[0] ||
            !strstr(field(utterance, "text")->valuestring, quote->valuestring) ||
            !strstr(quote->valuestring, name->valuestring)) {
            snprintf(error, capacity, "LLM speaker name lacks a matching quote from that speaker");
            cJSON_Delete(reply);
            return -1;
        }
    }
    for (int i = 0; i < count; ++i) {
        cJSON *utterance = cJSON_GetArrayItem(raw, first + i);
        cJSON_DeleteItemFromObjectCaseSensitive(utterance, "cleaned");
        cJSON_AddStringToObject(utterance, "cleaned", field(cJSON_GetArrayItem(cleaned, i), "text")->valuestring);
    }
    cJSON_ArrayForEach(intro, introductions) {
        cJSON *speaker = speaker_by_id(document, field(intro, "speaker")->valueint);
        if (strcmp(field(speaker, "source")->valuestring, "numbered") == 0) {
            set_name(speaker, field(intro, "name")->valuestring, "introduction_inferred");
            cJSON_AddStringToObject(speaker, "evidence", field(intro, "quote")->valuestring);
            cJSON_AddNumberToObject(speaker, "utterance", field(intro, "utterance")->valueint);
        }
    }
    cJSON_Delete(reply);
    return 0;
}

char *fb_meeting_document_part(const cJSON *document, int first, int count) {
    cJSON *part = cJSON_CreateObject();
    cJSON_AddItemToObject(part, "speakers", cJSON_Duplicate(field(document, "speakers"), 1));
    cJSON *utterances = cJSON_AddArrayToObject(part, "utterances");
    for (int i = first; i < first + count; ++i) {
        cJSON *item = cJSON_GetArrayItem(field(document, "utterances"), i);
        if (!item) {
            break;
        }
        cJSON_AddItemToArray(utterances, cJSON_Duplicate(item, 1));
    }
    char *result = cJSON_PrintUnformatted(part);
    cJSON_Delete(part);
    return result;
}

char *fb_meeting_document_text(const cJSON *document, int cleaned) {
    size_t length = 1;
    cJSON *utterance;
    cJSON_ArrayForEach(utterance, field(document, "utterances")) {
        cJSON *text = cleaned ? field(utterance, "cleaned") : NULL;
        if (!text) {
            text = field(utterance, "text");
        }
        cJSON *speaker = speaker_by_id(document, field(utterance, "speaker")->valueint);
        cJSON *name = speaker ? field(speaker, "name") : NULL;
        length += strlen(text->valuestring) + (name ? strlen(name->valuestring) : 32) + 128;
    }
    char *result = malloc(length);
    if (!result) {
        return NULL;
    }
    size_t offset = 0;
    result[0] = '\0';
    cJSON_ArrayForEach(utterance, field(document, "utterances")) {
        int id = field(utterance, "speaker")->valueint;
        cJSON *speaker = speaker_by_id(document, id);
        cJSON *name = speaker ? field(speaker, "name") : NULL;
        char numbered[32];
        snprintf(numbered, sizeof(numbered), "Speaker %d", id);
        const char *label = name ? name->valuestring : id ? numbered : "Unattributed";
        cJSON *text = cleaned ? field(utterance, "cleaned") : NULL;
        const char *pending = cleaned && !text ? "[ASR pending LLM] " : "";
        if (!text) {
            text = field(utterance, "text");
        }
        int written = snprintf(result + offset, length - offset, "[%.2f - %.2f] %s: %s%s\n",
                               field(utterance, "start")->valuedouble,
                               field(utterance, "end")->valuedouble, label, pending, text->valuestring);
        if (written > 0) {
            offset += (size_t)written;
        }
    }
    return result;
}
