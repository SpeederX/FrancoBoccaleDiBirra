#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "audio/wav_stream.h"
#include "audio/mic_capture.h"
#include "meeting/meeting_document.h"
#include "meeting/meeting_cli.h"
#include "net/sockets_compat.h"
#include "platform/platform.h"
#include "util/fileutil.h"
#include "util/paths.h"
#include "util/utf8.h"

static FbCell cells[36][120];
static int screen_width = 120;
static int screen_height = 36;

int fb_screen_init(void) { return 0; }
void fb_screen_shutdown(void) {}
int fb_screen_width(void) { return screen_width; }
int fb_screen_height(void) { return screen_height; }
void fb_screen_clear(void) { memset(cells, 0, sizeof(cells)); }
void fb_screen_present(void) {}
int fb_screen_poll_event(FbEvent *event, int timeout) {
    (void)event;
    (void)timeout;
    return 0;
}
void fb_screen_set_cell(int x, int y, const FbCell *cell) {
    if (x >= 0 && y >= 0 && x < screen_width && y < screen_height) {
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
    while (meeting->job || meeting->asr_job || meeting->save_job || meeting->devices_job || meeting->document_dirty) {
        fb_meeting_tick(meeting);
        assert(fb_now_ms() < deadline);
        fb_sleep_ms(5);
    }
}

static void resize(FbApp *app, int width, int height) {
    screen_width = width;
    screen_height = height;
    FbEvent event = {FB_EVENT_RESIZE, FB_KEY_NONE, 0, width, height};
    fb_app_handle_event(app, &event);
    fb_screen_clear();
    fb_app_render(app);
}

static void save_screen(const char *path) {
    FILE *screen = fopen(path, "wb");
    assert(screen);
    fprintf(screen, "{\"width\":%d,\"height\":%d,\"cells\":[", screen_width, screen_height);
    for (int y = 0; y < screen_height; ++y) {
        for (int x = 0; x < screen_width; ++x) {
            FbCell *cell = &cells[y][x];
            fprintf(screen, "%s[%u,%u,%u,%u,%u,%u,%u]", x || y ? "," : "",
                    cell->ch, cell->fg_r, cell->fg_g, cell->fg_b, cell->bg_r, cell->bg_g, cell->bg_b);
        }
    }
    fprintf(screen, "]}");
    fclose(screen);
}

static void test_transcript_viewport(FbApp *app) {
    app->meeting.llm_port = 0;
    size_t notes_revision = app->meeting.notes.revision;
    cJSON_Delete(app->meeting.document);
    char error[256];
    app->meeting.document = fb_meeting_document_parse(
        "{\"utterances\":[{\"speaker\":0,\"text\":\"first\",\"start\":0,\"end\":1}]}", error, sizeof(error));
    cJSON *utterances = cJSON_GetObjectItemCaseSensitive(app->meeting.document, "utterances");
    for (int i = 2; i <= 100; ++i) {
        cJSON *utterance = cJSON_Duplicate(cJSON_GetArrayItem(utterances, 0), 1);
        cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(utterance, "id"), i);
        char text[160];
        snprintf(text, sizeof(text), "Line %03d has enough words to wrap when the window becomes narrower.", i);
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(utterance, "text"), text);
        cJSON_AddItemToArray(utterances, utterance);
    }
    assert(fb_meeting_command(&app->meeting, "name 1 Reader") == 0);
    idle(&app->meeting);
    app->meeting.output_kind = 2;
    key(app, FB_KEY_F9, 0);
    resize(app, 120, 36);
    int visible = 28;
    assert(app->meeting.output.scroll == app->meeting.output.count - visible);
    assert(strstr(app->meeting.output.lines[app->meeting.output.count - 1], "narrower"));

    key(app, FB_KEY_PAGE_UP, 0);
    int reading = app->meeting.output.scroll;
    char top[FB_TEXTVIEW_LINE_MAX];
    snprintf(top, sizeof(top), "%s", app->meeting.output.lines[reading]);
    cJSON *latest = cJSON_Duplicate(cJSON_GetArrayItem(utterances, 99), 1);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(latest, "id"), 101);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(latest, "text"), "The latest transcript update.");
    cJSON_AddItemToArray(utterances, latest);
    assert(fb_meeting_command(&app->meeting, "name 1 Reader") == 0);
    idle(&app->meeting);
    assert(app->meeting.output.scroll > 0);
    assert(strcmp(app->meeting.output.lines[app->meeting.output.scroll], top) == 0);

    resize(app, 76, 22);
    save_screen("meeting-ui-narrow.json");
    assert(app->meeting.output.scroll > 0);
    assert(app->meeting.output.scroll < app->meeting.output.count - 14);
    for (int i = 0; i < app->meeting.output.count; ++i) {
        assert(strlen(app->meeting.output.lines[i]) <= 37);
    }
    for (int i = 0; i < 100; ++i) {
        key(app, FB_KEY_PAGE_DOWN, 0);
    }
    assert(app->meeting.output.scroll == app->meeting.output.count - 14);
    resize(app, 76, 30);
    assert(app->meeting.output.scroll == app->meeting.output.count - 22);
    resize(app, 120, 36);
    assert(app->meeting.output.scroll == app->meeting.output.count - visible);
    assert(strstr(app->meeting.output.lines[app->meeting.output.count - 1], "latest transcript"));
    assert(app->meeting.notes.revision == notes_revision);
}

static void begin_fixture_live(FbApp *app, const char *library) {
    assert(fb_meeting_command(&app->meeting, "new") == 0);
    idle(&app->meeting);
    snprintf(app->meeting.tools.parakeet_library, 512, "%s", library);
    snprintf(app->meeting.tools.asr_model, 512, "fixture");
    snprintf(app->meeting.tools.diar_model, 512, "fixture");
    char audio[700];
    snprintf(audio, sizeof(audio), "%s/audio.wav", app->meeting.directory);
    FbWavStream wav;
    assert(fb_wav_stream_open(&wav, audio, 16000) == 0);
    float *pcm = calloc(320000, sizeof(float));
    assert(pcm);
    assert(fb_wav_stream_append(&wav, pcm, 320000) == 0);
    assert(fb_wav_stream_append(&wav, pcm, 80000) == 0);
    assert(fb_wav_stream_close(&wav) == 0);
    free(pcm);
    app->meeting.asr_job = fb_meeting_job_start(FB_MEETING_JOB_LIVE, &app->meeting.tools,
        app->meeting.directory, NULL, NULL, NULL, 0, NULL);
    assert(app->meeting.asr_job);
    app->meeting.asr_live = 1;
    app->meeting.paused = 1;
    fb_notepad_set(&app->meeting.notes, "Final notes are still editable after closing.");
    app->meeting.command_focus = 0;
}

static int capture_smoke(int argc, char **argv) {
    assert(argc == 6);
    assert(fb_ensure_dir("capture-smoke-state") == 0);
#ifdef _WIN32
    assert(_putenv_s("LOCALAPPDATA", "capture-smoke-state") == 0);
#endif
    FbApp *app = calloc(1, sizeof(*app));
    assert(app);
    fb_app_init_mode(app, 120, 36, FB_INPUT_MODE_MEETING);
    snprintf(app->meeting.tools.parakeet_library, 512, "%s", argv[2]);
    snprintf(app->meeting.tools.asr_model, 512, "%s", argv[3]);
    snprintf(app->meeting.tools.diar_model, 512, "%s", argv[4]);
    snprintf(app->config.meeting_microphone, 256, "%s", argv[5]);
    assert(fb_meeting_command(&app->meeting, "record") == 0);
    unsigned long long deadline = fb_now_ms() + 20000;
    unsigned long long next_log = 0;
    while (app->meeting.capture_status.frames < 8000) {
        fb_app_tick(app);
        if (fb_now_ms() >= next_log) {
            FbMeetingJobStatus status = {0};
            if (app->meeting.asr_job) {
                fb_meeting_job_poll(app->meeting.asr_job, &status);
            }
            fprintf(stderr, "capture state=%d frames=%llu; ASR %s; UI %s\n",
                    app->meeting.capture_status.state, app->meeting.capture_status.frames,
                    status.message, app->meeting.status);
            fflush(stderr);
            next_log = fb_now_ms() + 1000;
        }
        assert(app->meeting.capture && fb_now_ms() < deadline);
        fb_sleep_ms(10);
    }
    key(app, FB_KEY_CTRL_SPACE, 0);
    deadline = fb_now_ms() + 15000;
    while (app->meeting.capture) {
        fb_app_tick(app);
        assert(fb_now_ms() < deadline);
        fb_sleep_ms(10);
    }
    assert(app->meeting.paused);
    unsigned long long recorded = app->meeting.capture_status.frames;
    key(app, FB_KEY_CTRL_SPACE, 0);
    deadline = fb_now_ms() + 15000;
    while (app->meeting.capture_status.frames <= recorded) {
        fb_app_tick(app);
        assert(app->meeting.capture && fb_now_ms() < deadline);
        fb_sleep_ms(10);
    }
    key(app, FB_KEY_F6, 0);
    deadline = fb_now_ms() + 30000;
    while (app->meeting.capture || app->meeting.asr_job || app->meeting.process_pending) {
        fb_app_tick(app);
        assert(fb_now_ms() < deadline);
        fb_sleep_ms(10);
    }
    assert(app->meeting.recording_finished && !app->meeting.paused);
    fprintf(stderr, "capture/pause/resume/F6 complete; frames=%llu; %s\n",
            app->meeting.capture_status.frames, app->meeting.status);
    fb_app_shutdown(app);
    free(app);
    return 0;
}

#ifdef FB_TEST_CAPTURE_FIXTURE
void fb_capture_fixture_ready(FbSystemCapture *capture, int fail);

static int capture_startup_test(const char *library) {
    assert(fb_ensure_dir("capture-startup-state") == 0);
#ifdef _WIN32
    assert(_putenv_s("LOCALAPPDATA", "capture-startup-state") == 0);
#endif
    FbApp *app = calloc(1, sizeof(*app));
    assert(app);
    fb_app_init_mode(app, 120, 36, FB_INPUT_MODE_MEETING);
    snprintf(app->meeting.tools.parakeet_library, 512, "%s", library);
    snprintf(app->meeting.tools.asr_model, 512, "fixture");
    snprintf(app->meeting.tools.diar_model, 512, "fixture");
    key(app, FB_KEY_CTRL_SPACE, 0);
    assert(app->meeting.capture && !app->meeting.asr_job);
    fb_app_tick(app);
    assert(!app->meeting.asr_job);
    key(app, FB_KEY_F6, 0);
    assert(app->meeting.finish_pending);
    fb_capture_fixture_ready(app->meeting.capture, 0);
    fb_app_tick(app);
    idle(&app->meeting);
    assert(app->meeting.recording_finished && !app->meeting.capture && !app->meeting.asr_job);

    assert(fb_meeting_command(&app->meeting, "new") == 0);
    idle(&app->meeting);
    assert(fb_meeting_command(&app->meeting, "record") == 0);
    fb_capture_fixture_ready(app->meeting.capture, 1);
    fb_app_tick(app);
    assert(!app->meeting.capture && !app->meeting.asr_job && !app->meeting.process_pending);
    assert(strstr(app->meeting.status, "device open failure"));

    assert(fb_meeting_command(&app->meeting, "new") == 0);
    idle(&app->meeting);
    key(app, FB_KEY_CTRL_SPACE, 0);
    key(app, FB_KEY_CTRL_SPACE, 0);
    fb_capture_fixture_ready(app->meeting.capture, 0);
    fb_app_tick(app);
    assert(app->meeting.paused && !app->meeting.capture && !app->meeting.asr_job);
    key(app, FB_KEY_CTRL_SPACE, 0);
    assert(app->meeting.capture && !app->meeting.asr_job);
    fb_capture_fixture_ready(app->meeting.capture, 0);
    fb_app_tick(app);
    assert(app->meeting.asr_job);
    assert(fb_meeting_command(&app->meeting, "finish") == 0);
    fb_app_tick(app);
    idle(&app->meeting);
    assert(app->meeting.recording_finished);

    assert(fb_meeting_command(&app->meeting, "new") == 0);
    idle(&app->meeting);
    key(app, FB_KEY_CTRL_SPACE, 0);
    assert(fb_meeting_command(&app->meeting, "cancel") == 0);
    fb_capture_fixture_ready(app->meeting.capture, 0);
    fb_app_tick(app);
    idle(&app->meeting);
    assert(app->meeting.recording_finished && !app->meeting.asr_job);

    assert(fb_meeting_command(&app->meeting, "new") == 0);
    idle(&app->meeting);
    assert(fb_meeting_command(&app->meeting, "record") == 0);
    fb_capture_fixture_ready(app->meeting.capture, 0);
    fb_app_tick(app);
    assert(app->meeting.asr_job && app->meeting.capture_status.frames == 8000);
    FbMeetingJob *session = app->meeting.asr_job;
    key(app, FB_KEY_CTRL_SPACE, 0);
    fb_app_tick(app);
    assert(app->meeting.paused && app->meeting.asr_job == session);
    key(app, FB_KEY_CTRL_SPACE, 0);
    assert(app->meeting.capture && app->meeting.asr_job == session);
    fb_capture_fixture_ready(app->meeting.capture, 0);
    fb_app_tick(app);
    assert(app->meeting.capture_status.frames == 16000);
    key(app, FB_KEY_F6, 0);
    fb_app_tick(app);
    idle(&app->meeting);
    assert(app->meeting.recording_finished && !app->meeting.paused);
    fb_app_shutdown(app);
    free(app);
    return 0;
}
#endif

int main(int argc, char **argv) {
    fb_sock_platform_init();
#ifdef FB_TEST_CAPTURE_FIXTURE
    assert(argc == 2);
    int result = capture_startup_test(argv[1]);
    fb_sock_platform_cleanup();
    return result;
#else
    if (argc > 1 && strcmp(argv[1], "--capture-smoke") == 0) {
        int result = capture_smoke(argc, argv);
        fb_sock_platform_cleanup();
        return result;
    }
#ifdef _WIN32
    if (argc == 2 && strcmp(argv[1], "--startup") == 0) {
        assert(fb_ensure_dir("test-startup-state") == 0);
        assert(_putenv_s("LOCALAPPDATA", "test-startup-state") == 0);
        FbApp *startup = calloc(1, sizeof(*startup));
        assert(startup);
        fb_app_init_mode(startup, 120, 36, FB_INPUT_MODE_MEETING);
        fb_app_tick(startup);
        assert(startup->input_mode == FB_INPUT_MODE_MEETING);
        assert(fb_mic_state() == FB_MIC_OFF);
        fb_app_shutdown(startup);
        free(startup);
        fb_sock_platform_cleanup();
        return 0;
    }
#endif
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
    assert(fb_meeting_command(&app->meeting, "microphone default") == 0);
    idle(&app->meeting);
    assert(app->meeting.settings_dirty && !app->config.meeting_microphone[0]);
    assert(fb_meeting_command(&app->meeting, "microphone nonexistent fixture microphone") == 0);
    idle(&app->meeting);
    assert(!app->config.meeting_microphone[0]);
    assert(strstr(app->meeting.status, "unavailable"));
    snprintf(app->config.meeting_microphone, 256, "Headset microphone");
    snprintf(app->config.meeting_output_device, 256, "Headphones output");
    assert(fb_config_save(&app->config, "test-meeting-state") == 0);
    assert(fb_config_load(&reopened, "test-meeting-state") == 0);
    assert(strcmp(reopened.meeting_microphone, "Headset microphone") == 0);
    assert(strcmp(reopened.meeting_output_device, "Headphones output") == 0);
    app->config.meeting_microphone[0] = 0;
    app->config.meeting_output_device[0] = 0;
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
        assert(!app->meeting.process_pending);
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
    if (argc >= 3 && strcmp(argv[1], "--live-hardware") != 0) {
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
    if (argc == 4) {
        assert(fb_meeting_command(&app->meeting, "finish") == -1);
        begin_fixture_live(app, argv[3]);
        key(app, FB_KEY_F6, 0);
        assert(app->meeting.recording_finished && !app->meeting.paused);
        assert(fb_meeting_command(&app->meeting, "finish") == -1);
        idle(&app->meeting);
        assert(app->input_mode == FB_INPUT_MODE_MEETING && app->running);
        assert(!app->meeting.command_focus);
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(app->meeting.document, "utterances")) == 2);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(app->meeting.document, "report_current")));
        assert(app->meeting.report && app->meeting.output_kind == 2);
        assert(app->meeting.operation == FB_MEETING_JOB_PROCESS);
        resize(app, 120, 36);
        save_screen("meeting-ui-closed.json");
        snprintf(path, sizeof(path), "%s/notes-used.txt", app->meeting.directory);
        saved = fb_read_file(path, NULL);
        assert(saved && strcmp(saved, app->meeting.notes.text) == 0);
        free(saved);
        type(app, " Added after the report.");
        assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(app->meeting.document, "report_current")));
        assert(fb_meeting_command(&app->meeting, "process") == 0);
        idle(&app->meeting);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(app->meeting.document, "report_current")));
        assert(fb_meeting_command(&app->meeting, "finish") == -1);
        begin_fixture_live(app, argv[3]);
        app->meeting.llm_port = 0;
        assert(fb_meeting_command(&app->meeting, "process") == 0);
        assert(app->meeting.finish_pending);
        idle(&app->meeting);
        assert(app->meeting.recording_finished && !app->meeting.job && !app->meeting.report);
        assert(strstr(app->meeting.status, "meeting closed") && strstr(app->meeting.status, "/meeting process"));
        assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(app->meeting.document, "utterances")) == 2);
    }
    if (argc == 5 && strcmp(argv[1], "--live-hardware") == 0) {
        assert(fb_meeting_command(&app->meeting, "new") == 0);
        idle(&app->meeting);
        snprintf(app->meeting.tools.parakeet_library, 512, "%s", argv[2]);
        snprintf(app->meeting.tools.asr_model, 512, "%s", argv[3]);
        snprintf(app->meeting.tools.diar_model, 512, "%s", argv[4]);
        assert(app->meeting.source == FB_CAPTURE_BOTH);
        key(app, FB_KEY_CTRL_SPACE, 0);
        assert(app->meeting.capture && !app->meeting.asr_job);
        unsigned long long opening_deadline = fb_now_ms() + 15000;
        while (!app->meeting.asr_job) {
            fb_meeting_tick(&app->meeting);
            assert(app->meeting.capture && fb_now_ms() < opening_deadline);
            fb_sleep_ms(10);
        }
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
    test_transcript_viewport(app);
    key(app, FB_KEY_F8, 0);
    fb_screen_clear();
    snprintf(app->meeting.status, sizeof(app->meeting.status), "notes saved locally; original transcript retained");
    fb_app_render(app);
    assert(app->meeting.output_width == 59);
    save_screen("meeting-ui.json");
    key(app, FB_KEY_TAB, 0);
    assert(app->input_mode == FB_INPUT_MODE_COMMAND);
    fb_meeting_shutdown(&app->meeting);
    free(app);
    fb_sock_platform_cleanup();
    return 0;
#endif
}
