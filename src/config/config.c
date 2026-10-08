#include "config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "platform/dirlist.h"
#include "util/fileutil.h"

static void build_path(char *out, size_t cap, const char *data_dir) {
    snprintf(out, cap, "%s/config.json", data_dir);
}

int fb_config_load(FbConfig *cfg, const char *data_dir) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->lang_pref, "en", sizeof(cfg->lang_pref) - 1);

    char path[600];
    build_path(path, sizeof(path), data_dir);

    char *text = fb_read_file(path, NULL);
    if (!text) return 0; /* no config yet: not an error */

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) return -1;

    cJSON *last_dir = cJSON_GetObjectItemCaseSensitive(root, "last_browse_dir");
    if (cJSON_IsString(last_dir)) {
        strncpy(cfg->last_browse_dir, last_dir->valuestring, sizeof(cfg->last_browse_dir) - 1);
    }
    cJSON *lang = cJSON_GetObjectItemCaseSensitive(root, "lang_pref");
    if (cJSON_IsString(lang)) {
        strncpy(cfg->lang_pref, lang->valuestring, sizeof(cfg->lang_pref) - 1);
        cfg->lang_pref[sizeof(cfg->lang_pref) - 1] = '\0';
    }

    struct { const char *key; char *dst; size_t cap; } str_fields[] = {
        {"llama_server_path", cfg->llama_server_path, sizeof(cfg->llama_server_path)},
        {"sherpa_exe", cfg->sherpa_exe, sizeof(cfg->sherpa_exe)},
        {"orukeet_dir", cfg->orukeet_dir, sizeof(cfg->orukeet_dir)},
        {"qwen_model", cfg->qwen_model, sizeof(cfg->qwen_model)},
        {"calibr_root", cfg->calibr_root, sizeof(cfg->calibr_root)},
        {"meeting_parakeet_exe", cfg->meeting_parakeet_exe, sizeof(cfg->meeting_parakeet_exe)},
        {"meeting_asr_model", cfg->meeting_asr_model, sizeof(cfg->meeting_asr_model)},
        {"meeting_diar_model", cfg->meeting_diar_model, sizeof(cfg->meeting_diar_model)},
        {"meeting_ffmpeg_exe", cfg->meeting_ffmpeg_exe, sizeof(cfg->meeting_ffmpeg_exe)},
        {"meeting_llm_model", cfg->meeting_llm_model, sizeof(cfg->meeting_llm_model)},
        {"meeting_folder", cfg->meeting_folder, sizeof(cfg->meeting_folder)},
    };
    for (size_t i = 0; i < sizeof(str_fields) / sizeof(str_fields[0]); ++i) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, str_fields[i].key);
        if (cJSON_IsString(v)) {
            strncpy(str_fields[i].dst, v->valuestring, str_fields[i].cap - 1);
            str_fields[i].dst[str_fields[i].cap - 1] = '\0';
        }
    }
    cJSON *qwen_port = cJSON_GetObjectItemCaseSensitive(root, "qwen_port");
    if (cJSON_IsNumber(qwen_port)) cfg->qwen_port = (int)qwen_port->valuedouble;

    struct { const char *key; int *dst; } int_fields[] = {
        {"diary_page_close_chars", &cfg->diary_page_close_chars},
        {"diary_checkpoint_chars", &cfg->diary_checkpoint_chars},
        {"diary_idle_close_minutes", &cfg->diary_idle_close_minutes},
        {"meeting_llm_port", &cfg->meeting_llm_port},
    };
    for (size_t i = 0; i < sizeof(int_fields) / sizeof(int_fields[0]); ++i) {
        cJSON *v = cJSON_GetObjectItemCaseSensitive(root, int_fields[i].key);
        if (cJSON_IsNumber(v)) *int_fields[i].dst = (int)v->valuedouble;
    }

    cJSON *models = cJSON_GetObjectItemCaseSensitive(root, "models");
    if (cJSON_IsArray(models)) {
        cJSON *item;
        cJSON_ArrayForEach(item, models) {
            if (cfg->model_count >= FB_CONFIG_MAX_MODELS) break;
            cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *model_path = cJSON_GetObjectItemCaseSensitive(item, "path");
            if (!cJSON_IsString(name) || !cJSON_IsString(model_path)) continue;

            FbConfigModel *m = &cfg->models[cfg->model_count++];
            strncpy(m->name, name->valuestring, sizeof(m->name) - 1);
            strncpy(m->path, model_path->valuestring, sizeof(m->path) - 1);
        }
    }

    cJSON_Delete(root);
    return 0;
}

int fb_config_save(const FbConfig *cfg, const char *data_dir) {
    char path[600];
    build_path(path, sizeof(path), data_dir);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "last_browse_dir", cfg->last_browse_dir);
    cJSON_AddStringToObject(root, "lang_pref", cfg->lang_pref);
    cJSON_AddStringToObject(root, "llama_server_path", cfg->llama_server_path);
    cJSON_AddStringToObject(root, "sherpa_exe", cfg->sherpa_exe);
    cJSON_AddStringToObject(root, "orukeet_dir", cfg->orukeet_dir);
    cJSON_AddStringToObject(root, "qwen_model", cfg->qwen_model);
    cJSON_AddNumberToObject(root, "qwen_port", cfg->qwen_port);
    cJSON_AddStringToObject(root, "calibr_root", cfg->calibr_root);
    cJSON_AddStringToObject(root, "meeting_parakeet_exe", cfg->meeting_parakeet_exe);
    cJSON_AddStringToObject(root, "meeting_asr_model", cfg->meeting_asr_model);
    cJSON_AddStringToObject(root, "meeting_diar_model", cfg->meeting_diar_model);
    cJSON_AddStringToObject(root, "meeting_ffmpeg_exe", cfg->meeting_ffmpeg_exe);
    cJSON_AddStringToObject(root, "meeting_llm_model", cfg->meeting_llm_model);
    cJSON_AddStringToObject(root, "meeting_folder", cfg->meeting_folder);
    cJSON_AddNumberToObject(root, "meeting_llm_port", cfg->meeting_llm_port);
    cJSON_AddNumberToObject(root, "diary_page_close_chars", cfg->diary_page_close_chars);
    cJSON_AddNumberToObject(root, "diary_checkpoint_chars", cfg->diary_checkpoint_chars);
    cJSON_AddNumberToObject(root, "diary_idle_close_minutes", cfg->diary_idle_close_minutes);
    cJSON *models = cJSON_AddArrayToObject(root, "models");
    for (int i = 0; i < cfg->model_count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", cfg->models[i].name);
        cJSON_AddStringToObject(item, "path", cfg->models[i].path);
        cJSON_AddItemToArray(models, item);
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text) return -1;

    FILE *f = fopen(path, "wb");
    if (!f) {
        cJSON_free(text);
        return -1;
    }
    fputs(text, f);
    fclose(f);
    cJSON_free(text);
    return 0;
}

int fb_config_find_model(const FbConfig *cfg, const char *name, char *out_path, size_t out_cap) {
    for (int i = 0; i < cfg->model_count; ++i) {
        if (strcmp(cfg->models[i].name, name) == 0) {
            snprintf(out_path, out_cap, "%s", cfg->models[i].path);
            return 1;
        }
    }
    return 0;
}

int fb_config_add_model(FbConfig *cfg, const char *name, const char *path) {
    for (int i = 0; i < cfg->model_count; ++i) {
        if (strcmp(cfg->models[i].name, name) == 0) {
            strncpy(cfg->models[i].path, path, sizeof(cfg->models[i].path) - 1);
            return 0;
        }
    }
    if (cfg->model_count >= FB_CONFIG_MAX_MODELS) return -1;

    FbConfigModel *m = &cfg->models[cfg->model_count++];
    strncpy(m->name, name, sizeof(m->name) - 1);
    strncpy(m->path, path, sizeof(m->path) - 1);
    return 0;
}

int fb_config_remove_model(FbConfig *cfg, const char *name) {
    for (int i = 0; i < cfg->model_count; ++i) {
        if (strcmp(cfg->models[i].name, name) != 0) continue;
        for (int j = i; j < cfg->model_count - 1; ++j) cfg->models[j] = cfg->models[j + 1];
        cfg->model_count--;
        return 0;
    }
    return -1;
}

int fb_resolve_llama_server(const FbConfig *cfg, char *out_path, size_t out_cap) {
    const char *src =
        cfg->llama_server_path[0] != '\0' ? cfg->llama_server_path : FB_LLAMA_SERVER_DEFAULT_NAME;
    snprintf(out_path, out_cap, "%s", src);
    return 0;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/* Bounded recursive search under <data_dir>/external/ -- where a first-run
   setup drops the sherpa-onnx SDK and Orukeet release archives -- so a
   fresh checkout with no env vars and no config.json entries still finds
   them. Depth/breadth caps are generous for this small, first-party-only
   folder (a real download tree is a handful of directories deep at most)
   without being unbounded. */
#define FB_AUTODETECT_MAX_DEPTH 6
#define FB_AUTODETECT_MAX_ENTRIES 128

static int find_file_under(const char *dir, const char *target_name, char *out, size_t out_cap,
                            int depth) {
    if (depth > FB_AUTODETECT_MAX_DEPTH) return 0;

    FbDirEntry entries[FB_AUTODETECT_MAX_ENTRIES];
    int n = fb_list_dir(dir, entries, FB_AUTODETECT_MAX_ENTRIES);
    if (n < 0) return 0;

    for (int i = 0; i < n; ++i) {
        if (!entries[i].is_dir && strcmp(entries[i].name, target_name) == 0) {
            snprintf(out, out_cap, "%s/%s", dir, entries[i].name);
            return 1;
        }
    }
    for (int i = 0; i < n; ++i) {
        /* fb_list_dir only excludes "." (the file picker needs ".." for its
           own up-navigation), so this walk must skip it itself or it
           wanders back out of external/ entirely. */
        if (!entries[i].is_dir || strcmp(entries[i].name, "..") == 0) continue;
        char sub[512];
        snprintf(sub, sizeof(sub), "%s/%s", dir, entries[i].name);
        if (find_file_under(sub, target_name, out, out_cap, depth + 1)) return 1;
    }
    return 0;
}

static int dir_has_orukeet_files(const char *dir) {
    char p[600];
    snprintf(p, sizeof(p), "%s/tokens.txt", dir);
    if (!file_exists(p)) return 0;
    snprintf(p, sizeof(p), "%s/encoder.int8.onnx", dir);
    if (!file_exists(p)) return 0;
    snprintf(p, sizeof(p), "%s/decoder.int8.onnx", dir);
    if (!file_exists(p)) return 0;
    snprintf(p, sizeof(p), "%s/joiner.int8.onnx", dir);
    return file_exists(p);
}

static int find_orukeet_dir_under(const char *dir, char *out, size_t out_cap, int depth) {
    if (dir_has_orukeet_files(dir)) {
        snprintf(out, out_cap, "%s", dir);
        return 1;
    }
    if (depth > FB_AUTODETECT_MAX_DEPTH) return 0;

    FbDirEntry entries[FB_AUTODETECT_MAX_ENTRIES];
    int n = fb_list_dir(dir, entries, FB_AUTODETECT_MAX_ENTRIES);
    if (n < 0) return 0;

    for (int i = 0; i < n; ++i) {
        if (!entries[i].is_dir || strcmp(entries[i].name, "..") == 0) continue;
        char sub[512];
        snprintf(sub, sizeof(sub), "%s/%s", dir, entries[i].name);
        if (find_orukeet_dir_under(sub, out, out_cap, depth + 1)) return 1;
    }
    return 0;
}

int fb_resolve_transcribe_config(const FbConfig *cfg, const char *data_dir, FbTranscribeConfig *out) {
    memset(out, 0, sizeof(*out));

    char external_dir[512];
    snprintf(external_dir, sizeof(external_dir), "%s/external", data_dir ? data_dir : "");

    if (cfg->sherpa_exe[0] != '\0') {
        snprintf(out->sherpa_exe, sizeof(out->sherpa_exe), "%s", cfg->sherpa_exe);
    } else if (data_dir && find_file_under(external_dir, FB_SHERPA_ONNX_DEFAULT_NAME, out->sherpa_exe,
                                            sizeof(out->sherpa_exe), 0)) {
        /* found under external/ */
    } else {
        snprintf(out->sherpa_exe, sizeof(out->sherpa_exe), "%s", FB_SHERPA_ONNX_DEFAULT_NAME);
    }

    char model_dir[512];
    model_dir[0] = '\0';
    if (cfg->orukeet_dir[0] != '\0') {
        snprintf(model_dir, sizeof(model_dir), "%s", cfg->orukeet_dir);
    } else if (data_dir) {
        find_orukeet_dir_under(external_dir, model_dir, sizeof(model_dir), 0);
    }
    if (model_dir[0] == '\0') return -1;

    snprintf(out->tokens_path, sizeof(out->tokens_path), "%s/tokens.txt", model_dir);
    snprintf(out->encoder_path, sizeof(out->encoder_path), "%s/encoder.int8.onnx", model_dir);
    snprintf(out->decoder_path, sizeof(out->decoder_path), "%s/decoder.int8.onnx", model_dir);
    snprintf(out->joiner_path, sizeof(out->joiner_path), "%s/joiner.int8.onnx", model_dir);

    if (!file_exists(out->tokens_path) || !file_exists(out->encoder_path) ||
        !file_exists(out->decoder_path) || !file_exists(out->joiner_path)) {
        return -1;
    }
    return 0;
}

int fb_resolve_qwen_model(const FbConfig *cfg, char *out_path, size_t cap) {
    if (cfg->qwen_model[0] == '\0' || !file_exists(cfg->qwen_model)) return -1;
    int n = snprintf(out_path, cap, "%s", cfg->qwen_model);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int fb_resolve_qwen_port(const FbConfig *cfg) {
    if (cfg->qwen_port > 0 && cfg->qwen_port < 65536) return cfg->qwen_port;
    return FB_QWEN_DEFAULT_PORT;
}
