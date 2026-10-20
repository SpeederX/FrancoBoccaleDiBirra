#include "tui/widget_notepad.h"

#include <assert.h>
#include <string.h>

void fb_screen_set_cell(int x, int y, const FbCell *cell) {
    (void)x;
    (void)y;
    (void)cell;
}

static void key(FbNotepad *pad, FbKeyCode code, uint32_t ch) {
    FbEvent event = {FB_EVENT_KEY, code, ch, 0, 0};
    assert(fb_notepad_handle_event(pad, &event) >= 0);
}

int main(void) {
    FbNotepad pad;
    fb_notepad_init(&pad);
    key(&pad, FB_KEY_BACKSPACE, 0);
    key(&pad, FB_KEY_CHAR, 0);
    assert(pad.len == 0);
    key(&pad, FB_KEY_CHAR, 'a');
    key(&pad, FB_KEY_CHAR, 0xE8);
    key(&pad, FB_KEY_ENTER, 0);
    key(&pad, FB_KEY_CHAR, 'b');
    assert(strcmp(pad.text, "a\xC3\xA8\nb") == 0);
    key(&pad, FB_KEY_ARROW_UP, 0);
    assert(pad.cursor == 1);
    key(&pad, FB_KEY_ARROW_RIGHT, 0);
    assert(pad.cursor == 3);
    key(&pad, FB_KEY_BACKSPACE, 0);
    assert(strcmp(pad.text, "a\nb") == 0);
    key(&pad, FB_KEY_ARROW_DOWN, 0);
    key(&pad, FB_KEY_CHAR, 'c');
    assert(strcmp(pad.text, "a\nbc") == 0);
    for (int i = 0; i < 5000; ++i) {
        key(&pad, FB_KEY_CHAR, 'x');
    }
    assert(pad.len == 5004);
    FbRect area = {0, 0, 30, 3};
    fb_notepad_render(&pad, area, 1);
    assert(pad.scroll > 0);
    assert(fb_notepad_set(&pad, "new\nnotes") == 0);
    assert(pad.cursor == pad.len && pad.scroll == 0);
    FbEvent resize = {FB_EVENT_RESIZE, FB_KEY_CHAR, 'z', 0, 0};
    assert(fb_notepad_handle_event(&pad, &resize) == 0);
    assert(strcmp(pad.text, "new\nnotes") == 0);
    fb_notepad_free(&pad);
    return 0;
}
