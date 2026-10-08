#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "meeting/meeting_document.h"
#include "meeting/meeting_cli.h"
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
    while (meeting->job || meeting->save_job) {
        fb_meeting_tick(meeting);
        assert(fb_now_ms() < deadline);
        fb_sleep_ms(5);
    }
}

int main(int argc, char **argv) {
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
    fb_meeting_init(&app->meeting, &app->config, "test-meeting-state");
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
    if (argc > 1) {
        snprintf(app->meeting.tools.parakeet_exe, 512, "%s", argv[1]);
        snprintf(app->meeting.tools.asr_model, 512, "slow");
        snprintf(app->meeting.tools.diar_model, 512, "fixture");
        assert(fb_meeting_command(&app->meeting, "transcribe") == 0);
        fb_sleep_ms(80);
        assert(app->meeting.job);
        key(app, FB_KEY_F8, 0);
        type(app, " Notes added during transcription.");
        assert(strstr(app->meeting.notes.text, "during transcription"));
        assert(fb_meeting_command(&app->meeting, "new") == -1);
        assert(fb_meeting_command(&app->meeting, "cancel") == 0);
        unsigned long long before = fb_now_ms();
        idle(&app->meeting);
        assert(fb_now_ms() - before < 2000);
        assert(strstr(app->meeting.status, "cancelled"));
        assert(app->meeting.document && strstr(app->meeting.notes.text, "during transcription"));
        assert(fb_meeting_command(&app->meeting, "save") == 0);
        idle(&app->meeting);
        key(app, FB_KEY_F8, 0);
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
    return 0;
}
