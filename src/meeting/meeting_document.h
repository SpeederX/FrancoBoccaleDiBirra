#ifndef FB_MEETING_DOCUMENT_H
#define FB_MEETING_DOCUMENT_H

#include <stddef.h>
#include "cJSON.h"

cJSON *fb_meeting_document_parse(const char *scene_jsonl, char *error, size_t capacity);
int fb_meeting_document_valid(const cJSON *document);
int fb_meeting_document_preserve_names(cJSON *document, const cJSON *previous);
int fb_meeting_document_append(cJSON *document, const char *scene, char *error, size_t capacity);
void fb_meeting_document_overlay(cJSON *document, const cJSON *previous);
int fb_meeting_document_name(cJSON *document, int speaker, const char *name,
                             char *error, size_t capacity);
int fb_meeting_document_apply(cJSON *document, const char *response, int first, int count,
                              char *error, size_t capacity);
char *fb_meeting_document_text(const cJSON *document, int cleaned);
char *fb_meeting_document_speech(const cJSON *document);
char *fb_meeting_document_part(const cJSON *document, int first, int count);

#endif
