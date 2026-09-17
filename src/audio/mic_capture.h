#ifndef FB_MIC_CAPTURE_H
#define FB_MIC_CAPTURE_H

#include <stddef.h>

/* Orukeet/sherpa-onnx (milestone 6) expects 16kHz mono; capturing directly
   at that rate avoids a resample step later. */
#define FB_MIC_SAMPLE_RATE 16000

int fb_mic_init(void);
void fb_mic_shutdown(void);

/* Drains newly available metering levels (RMS amplitude in [0,1], oldest
   first) into out[], up to max_count. Returns the number written. Call
   once per UI frame. */
size_t fb_mic_poll_levels(float *out, size_t max_count);

/* Starts (1) or stops (0) accumulating raw mono float samples at
   FB_MIC_SAMPLE_RATE for later transcription. Starting resets any
   previously accumulated recording. */
void fb_mic_set_recording(int recording);

/* Valid once fb_mic_set_recording(0) has been called: pointer + count of
   the samples accumulated during the last recording. Owned internally,
   valid until the next fb_mic_set_recording(1) call. Recordings longer
   than ~30s are truncated rather than growing unbounded. */
const float *fb_mic_recorded_samples(size_t *out_count);

#endif /* FB_MIC_CAPTURE_H */
