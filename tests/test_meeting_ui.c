#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "meeting/meeting_document.h"
#include "meeting/meeting_cli.h"
#include "net/sockets_compat.h"
#include "platform/platform.h"
#include "util/fileutil.h"
#include "util/paths.h"
#include "util/utf8.h"

static FbCell cells[36][120];

int fb_screen_init(void) { return 0; }
void fb_screen_shutdown(void) {}
int fb_screen_width(void) { return 120; }
int fb_screen_height(void) { return 36; }
void fb_screen_clear(void) { memset(cells, 0, sizeof(cells)); }
void fb_screen_present(void) {}
int fb_screen_poll_event(FbEvent *event, int timeout) {
    (void)event;
    (void)timeout;
    return 0;
}
void fb_screen_set_cell(int x, int y, const FbCell *cell) {
    if (x >= 0 && y >= 0 && x < 120 && y < 36) {
        cells[y][x] = *cell;
    }
}

static void key(FbApp *app, FbKeyCode code, uint32_t ch) {
    FbEvent event = {FB_EVENT_KEY, code, ch, 0, 0};
    fb_app_handle_event(app, &event);
}

static void type(FbApp *app, const char *text) {
    while (*text) {
        uint32_t cp;
        text += fb_utf8_decode(text, &cp);
        key(app, FB_KEY_CHAR, cp);
    }
}

static void idle(FbMeeting *meeting) {
    unsigned long long deadline = fb_now_ms() + 5000;
    while (meeting->job || meeting->asr_job || meeting->save_job || meeting->document_dirty) {
        fb_meeting_tick(meeting);
        assert(fb_now_ms() < deadline);
        fb_sleep_ms(5);
    }
}

int main(int argc, char **argv) {
    fb_sock_platform_init();
    FbConfig settings = {0};
    char *launch[] = {"fbdb", "--meeting", "--parakeet", "C:/tools with spaces/parakeet-cli.exe",
                      "--asr", "italian.gguf", "--llm-port", "18089", "--llm-model", "exact-model"};
    assert(fb_meeting_launch_config(10, launch, &settings) == 1);
    assert(strcmp(settings.meeting_parakeet_exe, launch[3]) == 0);
    assert(strcmp(settings.meeting_asr_model, "italian.gguf") == 0);
    assert(strcmp(settings.meeting_llm_model, "exact-model") == 0);
    assert(settings.meeting_llm_port == 18089);
    assert(fb_meeting_launch_config(3, launch, &settings) == -1);
    char *bad_launch[] = {"fbdb", "--meeting", "--llm-port", "8080junk"};
    assert(fb_meeting_launch_config(4, bad_launch, &settings) == -1);
    char *unknown_launch[] = {"fbdb", "--meeting", "--unknown", "value"};
    assert(fb_meeting_launch_config(4, unknown_launch, &settings) == -1);
    assert(fb_meeting_launch_config(1, launch, &settings) == 0);
    assert(fb_ensure_dir("test-meeting-state") == 0);
    FbApp *app = calloc(1, sizeof(*app));
    assert(app);
    app->running = 1;
    app->width = 120;
    app->height = 36;
    fb_cmdline_init(&app->cmdline);
    fb_meeting_init(&app->meeting, &app->config);
    char executable_dir[512];
    assert(fb_get_executable_dir(executable_dir, sizeof(executable_dir)) == 0);
    assert(strstr(app->meeting.root, executable_dir));
    assert(strstr(app->meeting.root, "/meetings"));
    assert(fb_meeting_command(&app->meeting, "folder test-meeting-state/meetings") == 1);
    assert(strcmp(app->config.meeting_folder, "test-meeting-state/meetings") == 0);
    assert(fb_config_save(&app->config, "test-meeting-state") == 0);
    FbConfig reopened = {0};
    assert(fb_config_load(&reopened, "test-meeting-state") == 0);
    assert(strcmp(reopened.meeting_folder, app->config.meeting_folder) == 0);
    assert(fb_meeting_command(&app->meeting, "folder nonexistent-parent/subfolder") == -1);
    assert(app->meeting.source == FB_CAPTURE_BOTH);
    assert(fb_meeting_command(&app->meeting, "source unknown") == -1);
    assert(fb_meeting_command(&app->meeting, "source microphone") == 1);
    assert(app->meeting.source == FB_CAPTURE_MICROPHONE);
    assert(fb_config_save(&app->config, "test-meeting-state") == 0);
    assert(fb_config_load(&reopened, "test-meeting-state") == 0);
    assert(reopened.meeting_capture_source == FB_CAPTURE_MICROPHONE);
    assert(fb_meeting_command(&app->meeting, "source both") == 1);
    key(app, FB_KEY_TAB, 0);
    assert(app->input_mode == FB_INPUT_MODE_SCRATCHPAD);
    key(app, FB_KEY_TAB, 0);
    assert(app->input_mode == FB_INPUT_MODE_DIARY);
    key(app, FB_KEY_TAB, 0);
    assert(app->input_mode == FB_INPUT_MODE_MEETING);
    key(app, FB_KEY_F1, 0);
    assert(app->meeting.command_focus);
    assert(strstr(app->meeting.output_text, "/meeting setup"));
    type(app, "/meeting new");
    key(app, FB_KEY_ENTER, 0);
    idle(&app->meeting);
    assert(app->meeting.directory[0]);
    assert(!app->meeting.command_focus);
    type(app, "Review the draft with Alice.");
    key(app, FB_KEY_ENTER, 0);
    type(app, "Deadline: Friday. Owner: Bob.");
    assert(strchr(app->meeting.notes.text, '\n'));
    key(app, FB_KEY_F8, 0);
    type(app, "/meeting save");
    key(app, FB_KEY_ENTER, 0);
    idle(&app->meeting);
    char path[700];
    snprintf(path, sizeof(path), "%s/notes.txt", app->meeting.directory);
    char *saved = fb_read_file(path, NULL);
    assert(saved && strcmp(saved, app->meeting.notes.text) == 0);
    free(saved);
    char error[256];
    app->meeting.document = fb_meeting_document_parse(
        "{\"utterances\":[{\"speaker\":0,\"text\":\"My name is Alice. I will review the draft.\",\"start\":0,\"end\":4},"
        "{\"speaker\":1,\"text\":\"I will send it by Friday.\",\"start\":4,\"end\":7}]}", error, sizeof(error));
    assert(app->meeting.document);
    assert(fb_meeting_command(&app->meeting, "name 1 Alice") == 0);
    idle(&app->meeting);
    assert(fb_meeting_command(&app->meeting, "name 99 Nobody") == -1);
    assert(fb_meeting_command(&app->meeting, "name 2Carol") == -1);
    assert(fb_meeting_command(&app->meeting, "model 8080junk x") == -1);
    assert(fb_meeting_command(&app->meeting, "process extra") == -1);
    assert(fb_meeting_command(&app->meeting, "setup unknown x") == -1);
    key(app, FB_KEY_F9, 0);
    assert(app->meeting.output_kind == 1);
    assert(strstr(app->meeting.output_text, "Alice"));
    char opening[700];
    snprintf(opening, sizeof(opening), "open %s", app->meeting.directory);
    assert(fb_meeting_command(&app->meeting, opening) == 0);
    idle(&app->meeting);
    assert(strstr(app->meeting.notes.text, "Friday"));
    assert(strstr(app->meeting.output_text, "Alice"));
    assert(fb_meeting_command(&app->meeting, "open nonexistent-meeting-folder") == 0);
    idle(&app->meeting);
    assert(app->meeting.document && strstr(app->meeting.notes.text, "Friday"));
    if (argc > 1 && strcmp(argv[1], "--live-hardware") != 0) {
        snprintf(app->meeting.tools.parakeet_exe, 512, "%s", argv[1]);
        snprintf(app->meeting.tools.asr_model, 512, "slow");
        snprintf(app->meeting.tools.diar_model, 512, "fixture");
        assert(fb_meeting_command(&app->meeting, "transcribe") == 0);
        fb_sleep_ms(80);
        assert(app->meeting.asr_job);
        key(app, FB_KEY_F8, 0);
        type(app, " Notes added during transcription.");
        assert(strstr(app->meeting.notes.text, "during transcription"));
        assert(fb_meeting_command(&app->meeting, "new") == -1);
        app->meeting.asr_live = 1;
        assert(fb_meeting_command(&app->meeting, "cancel") == 0);
        assert(app->meeting.recording_finished && !app->meeting.paused);
        unsigned long long before = fb_now_ms();
        idle(&app->meeting);
        assert(fb_now_ms() - before < 2000);
        assert(strstr(app->meeting.status, "cancelled"));
        assert(app->meeting.document && strstr(app->meeting.notes.text, "during transcription"));
        assert(fb_meeting_command(&app->meeting, "save") == 0);
        idle(&app->meeting);
        key(app, FB_KEY_F8, 0);
    }
    if (argc == 3) {
        cJSON_Delete(app->meeting.document);
        app->meeting.document = fb_meeting_document_parse(
            "{\"utterances\":[{\"speaker\":3,\"text\":\"Mario, puoi mandare il documento?\",\"start\":0,\"end\":2},"
            "{\"speaker\":7,\"text\":\"Sì, lo mando domani.\",\"start\":2,\"end\":4},"
            "{\"speaker\":3,\"text\":\"Giulia è assente oggi.\",\"start\":4,\"end\":6}]}", error, sizeof(error));
        assert(app->meeting.document);
        app->meeting.llm_port = atoi(argv[2]);
        snprintf(app->meeting.llm_model, sizeof(app->meeting.llm_model), "exact-fixture-model");
        app->meeting.identity_done = 0;
        fb_meeting_tick(&app->meeting);
        idle(&app->meeting);
        char *named = fb_meeting_document_text(app->meeting.document, 0);
        assert(strstr(named, "Mario: Sì"));
        assert(!strstr(named, "Giulia:"));
        free(named);
        cJSON *spoken = cJSON_GetObjectItemCaseSensitive(app->meeting.document, "utterances");
        assert(!cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(spoken, 1), "cleaned"));
        cJSON_AddStringToObject(cJSON_GetArrayItem(spoken, 1), "cleaned", "Sì, lo invio domani.");
        assert(fb_meeting_command(&app->meeting, "speakers") == 0);
        assert(strstr(app->meeting.output_text, "inferred from addressed name"));
        assert(fb_meeting_command(&app->meeting, "identify") == 0);
        assert(fb_meeting_command(&app->meeting, "name 2 Marco") == 0);
        idle(&app->meeting);
        named = fb_meeting_document_text(app->meeting.document, 0);
        assert(strstr(named, "Marco: Sì") && !strstr(named, "Mario: Sì"));
        free(named);
        spoken = cJSON_GetObjectItemCaseSensitive(app->meeting.document, "utterances");
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(spoken, 1), "cleaned")->valuestring,
                      "Sì, lo invio domani.") == 0);
    }
    if (argc == 5 && strcmp(argv[1], "--live-hardware") == 0) {
        assert(fb_meeting_command(&app->meeting, "new") == 0);
        idle(&app->meeting);
        snprintf(app->meeting.tools.parakeet_library, 512, "%s", argv[2]);
        snprintf(app->meeting.tools.asr_model, 512, "%s", argv[3]);
        snprintf(app->meeting.tools.diar_model, 512, "%s", argv[4]);
        assert(app->meeting.source == FB_CAPTURE_BOTH);
        key(app, FB_KEY_CTRL_SPACE, 0);
        assert(app->meeting.capture && app->meeting.asr_job);
        FbMeetingJob *session = app->meeting.asr_job;
        char recording_directory[600];
        snprintf(recording_directory, sizeof(recording_directory), "%s", app->meeting.directory);
        type(app, "Notes during live capture.");
        unsigned long long deadline = fb_now_ms() + 60000;
        while (!app->meeting.document ||
               cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(app->meeting.document, "utterances")) == 0) {
            fb_meeting_tick(&app->meeting);
            assert(app->meeting.asr_job && fb_now_ms() < deadline);
            fb_sleep_ms(10);
        }
        assert(app->meeting.capture);
        assert(app->meeting.output_text && strstr(app->meeting.output_text, "Speaker"));
        key(app, FB_KEY_CTRL_SPACE, 0);
        deadline = fb_now_ms() + 15000;
        while (app->meeting.capture) {
            fb_meeting_tick(&app->meeting);
            assert(fb_now_ms() < deadline);
            fb_sleep_ms(10);
        }
        assert(app->meeting.paused && app->meeting.asr_job == session);
        unsigned long long before_resume = app->meeting.capture_status.frames;
        key(app, FB_KEY_CTRL_SPACE, 0);
        assert(app->meeting.capture && app->meeting.asr_job == session);
        assert(strcmp(app->meeting.directory, recording_directory) == 0);
        type(app, " More notes after resume.");
        deadline = fb_now_ms() + 3000;
        while (fb_now_ms() < deadline) {
            fb_meeting_tick(&app->meeting);
            fb_sleep_ms(10);
        }
        key(app, FB_KEY_CTRL_SPACE, 0);
        deadline = fb_now_ms() + 15000;
        while (app->meeting.capture) {
            fb_meeting_tick(&app->meeting);
            assert(fb_now_ms() < deadline);
            fb_sleep_ms(10);
        }
        assert(app->meeting.paused && app->meeting.capture_status.frames > before_resume);
        assert(fb_meeting_command(&app->meeting, "finish") == 0);
        deadline = fb_now_ms() + 60000;
        while (app->meeting.asr_job || app->meeting.save_job || app->meeting.document_dirty) {
            fb_meeting_tick(&app->meeting);
            assert(fb_now_ms() < deadline);
            fb_sleep_ms(10);
        }
        assert(app->meeting.recording_finished && !app->meeting.paused);
        assert(strstr(app->meeting.notes.text, "after resume"));
        printf("live qualification: %llu frames, %llu dropped, %d utterances, same session and folder\n",
            app->meeting.capture_status.frames, app->meeting.capture_status.dropped_frames,
            cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(app->meeting.document, "utterances")));
    }
    key(app, FB_KEY_PAGE_DOWN, 0);
    assert(app->meeting.output.scroll > 0);
    key(app, FB_KEY_PAGE_UP, 0);
    assert(app->meeting.output.scroll == 0);
    key(app, FB_KEY_F8, 0);
    fb_screen_clear();
    snprintf(app->meeting.status, sizeof(app->meeting.status), "notes saved locally; original transcript retained");
    fb_app_render(app);
    assert(app->meeting.output_width == 59);
    FILE *screen = fopen("meeting-ui.json", "wb");
    assert(screen);
    fprintf(screen, "{\"width\":120,\"height\":36,\"cells\":[");
    for (int y = 0; y < 36; ++y) {
        for (int x = 0; x < 120; ++x) {
            FbCell *cell = &cells[y][x];
            fprintf(screen, "%s[%u,%u,%u,%u,%u,%u,%u]", x || y ? "," : "",
                    cell->ch, cell->fg_r, cell->fg_g, cell->fg_b, cell->bg_r, cell->bg_g, cell->bg_b);
        }
    }
    fprintf(screen, "]}");
    fclose(screen);
    key(app, FB_KEY_TAB, 0);
    assert(app->input_mode == FB_INPUT_MODE_COMMAND);
    fb_meeting_shutdown(&app->meeting);
    free(app);
    fb_sock_platform_cleanup();
    return 0;
}
