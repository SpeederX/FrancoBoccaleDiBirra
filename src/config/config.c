#include "config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
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

int fb_resolve_llama_server(char *out_path, size_t out_cap) {
    const char *env = getenv("FB_LLAMA_SERVER");
    if (env && env[0] != '\0') {
        snprintf(out_path, out_cap, "%s", env);
    } else {
        snprintf(out_path, out_cap, "%s", FB_LLAMA_SERVER_DEFAULT_NAME);
    }
    return 0;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int fb_resolve_transcribe_config(FbTranscribeConfig *out) {
    memset(out, 0, sizeof(*out));

    const char *sherpa_env = getenv("FB_SHERPA_ONNX_EXE");
    if (sherpa_env && sherpa_env[0] != '\0') {
        snprintf(out->sherpa_exe, sizeof(out->sherpa_exe), "%s", sherpa_env);
    } else {
        snprintf(out->sherpa_exe, sizeof(out->sherpa_exe), "%s", FB_SHERPA_ONNX_DEFAULT_NAME);
    }

    const char *model_dir = getenv("FB_ORUKEET_DIR");
    if (!model_dir || model_dir[0] == '\0') return -1;

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

int fb_resolve_qwen_model(char *out_path, size_t cap) {
    const char *env = getenv("FB_QWEN_MODEL");
    if (!env || env[0] == '\0' || !file_exists(env)) return -1;
    int n = snprintf(out_path, cap, "%s", env);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

int fb_resolve_qwen_port(void) {
    const char *env = getenv("FB_QWEN_PORT");
    if (env && env[0] != '\0') {
        int p = atoi(env);
        if (p > 0 && p < 65536) return p;
    }
    return FB_QWEN_DEFAULT_PORT;
}
