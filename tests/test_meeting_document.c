#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "meeting/meeting_document.h"

static const char *scene =
    "{\"utterances\":[]}\n"
    "{\"utterances\":[{\"speaker\":3,\"text\":\"Hi, my name is Alice.\",\"start\":0.1,\"end\":2.0},"
    "{\"speaker\":0,\"text\":\"Bob will send the draft.\",\"start\":2.1,\"end\":3.0},"
    "{\"speaker\":3,\"text\":\"I will review it.\",\"start\":3.1,\"end\":4.0},"
    "{\"speaker\":-1,\"text\":\"An overlapping voice.\",\"start\":3.5,\"end\":4.0}]}\n";

int main(void) {
    char error[256];
    assert(!fb_meeting_document_parse("broken", error, sizeof(error)));
    assert(!fb_meeting_document_parse("{\"utterances\":null}", error, sizeof(error)));
    assert(!fb_meeting_document_parse("{\"utterances\":[{\"speaker\":1,\"text\":\"x\",\"start\":2,\"end\":1}]}", error, sizeof(error)));
    assert(!fb_meeting_document_parse("{\"utterances\":[{\"speaker\":1.2,\"text\":\"x\",\"start\":0,\"end\":1}]}", error, sizeof(error)));
    cJSON *doc = fb_meeting_document_parse(scene, error, sizeof(error));
    assert(doc);
    assert(fb_meeting_document_valid(doc));
    cJSON *invalid = cJSON_Parse("{\"speakers\":[],\"utterances\":[{}]}");
    assert(!fb_meeting_document_valid(invalid));
    cJSON_Delete(invalid);
    cJSON *speakers = cJSON_GetObjectItemCaseSensitive(doc, "speakers");
    assert(cJSON_GetArraySize(speakers) == 2);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(speakers, 0), "slot")->valueint == 3);
    char *text = fb_meeting_document_text(doc, 0);
    assert(strstr(text, "Speaker 1") && strstr(text, "Speaker 2") && strstr(text, "Unattributed"));
    free(text);
    assert(fb_meeting_document_name(doc, 99, "Nobody", error, sizeof(error)) == -1);
    assert(fb_meeting_document_name(doc, 1, "", error, sizeof(error)) == -1);
    const char *valid = "{\"utterances\":[{\"id\":1,\"text\":\"Hi, my name is Alice.\"},{\"id\":2,\"text\":\"Bob will send the draft.\"}],"
        "\"introductions\":[{\"speaker\":1,\"name\":\"Alice\",\"utterance\":1,\"quote\":\"my name is Alice\"}]}";
    assert(fb_meeting_document_apply(doc, "{}", 0, 2, error, sizeof(error)) == -1);
    assert(fb_meeting_document_apply(doc, "{\"utterances\":[{\"id\":1,\"text\":\"one\"},{\"id\":1,\"text\":\"duplicate\"}]}", 0, 2, error, sizeof(error)) == -1);
    assert(fb_meeting_document_apply(doc, valid, 0, 2, error, sizeof(error)) == 0);
    text = fb_meeting_document_text(doc, 1);
    assert(strstr(text, "Alice") && strstr(text, "Speaker 2"));
    free(text);
    assert(fb_meeting_document_name(doc, 1, "Alicia", error, sizeof(error)) == 0);
    assert(fb_meeting_document_apply(doc, valid, 0, 2, error, sizeof(error)) == 0);
    text = fb_meeting_document_text(doc, 1);
    assert(strstr(text, "Alicia"));
    free(text);
    cJSON *retry = fb_meeting_document_parse(scene, error, sizeof(error));
    assert(fb_meeting_document_preserve_names(retry, doc) == 1);
    text = fb_meeting_document_text(retry, 0);
    assert(strstr(text, "Alicia"));
    free(text);
    cJSON_Delete(retry);
    retry = fb_meeting_document_parse(scene, error, sizeof(error));
    cJSON *changed = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(retry, "utterances"), 0);
    cJSON_ReplaceItemInObjectCaseSensitive(changed, "text", cJSON_CreateString("Changed ASR"));
    assert(fb_meeting_document_preserve_names(retry, doc) == 0);
    assert(!cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(retry, "speakers"), 0), "name"));
    cJSON_Delete(retry);
    const char *fake = "{\"utterances\":[{\"id\":3,\"text\":\"I will review it.\"}],"
        "\"introductions\":[{\"speaker\":2,\"name\":\"Bob\",\"utterance\":3,\"quote\":\"I am Bob\"}]}";
    assert(fb_meeting_document_apply(doc, fake, 2, 1, error, sizeof(error)) == -1);
    cJSON *third = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(doc, "utterances"), 2);
    assert(!cJSON_GetObjectItemCaseSensitive(third, "cleaned"));
    char *part = fb_meeting_document_part(doc, 2, 2);
    assert(part && strstr(part, "\"id\":3") && strstr(part, "\"id\":4"));
    free(part);
    cJSON_Delete(doc);
    doc = fb_meeting_document_parse("{\"utterances\":[]}", error, sizeof(error));
    assert(doc);
    cJSON_Delete(doc);
    return 0;
}
