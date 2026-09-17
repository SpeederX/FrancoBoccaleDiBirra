#include <assert.h>
#include <stdio.h>

#include "audio/ring_buffer.h"

int main(void) {
    FbRingBuffer rb;
    assert(fb_ring_buffer_init(&rb, 4) == 0);
    assert(fb_ring_buffer_available(&rb) == 0);

    float in3[3] = {1.0f, 2.0f, 3.0f};
    assert(fb_ring_buffer_write(&rb, in3, 3) == 3);
    assert(fb_ring_buffer_available(&rb) == 3);

    /* Buffer capacity is 4; only 1 more slot free. */
    float in5[5] = {4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
    assert(fb_ring_buffer_write(&rb, in5, 5) == 1);
    assert(fb_ring_buffer_available(&rb) == 4);

    float out[4] = {0};
    assert(fb_ring_buffer_read(&rb, out, 4) == 4);
    assert(out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f && out[3] == 4.0f);
    assert(fb_ring_buffer_available(&rb) == 0);

    /* Wraparound: write/read repeatedly past the underlying array length. */
    for (int round = 0; round < 10; ++round) {
        float v[2] = {(float)round, (float)round + 0.5f};
        assert(fb_ring_buffer_write(&rb, v, 2) == 2);
        float o[2];
        assert(fb_ring_buffer_read(&rb, o, 2) == 2);
        assert(o[0] == v[0] && o[1] == v[1]);
    }

    fb_ring_buffer_free(&rb);
    printf("test_ring_buffer: OK\n");
    return 0;
}
