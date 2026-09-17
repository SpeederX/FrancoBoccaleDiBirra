#ifndef FB_MIC_CAPTURE_H
#define FB_MIC_CAPTURE_H

#include <stddef.h>

/* Orukeet/sherpa-onnx (milestone 6) expects 16kHz mono; capturing directly
   at that rate avoids a resample step later. */
#define FB_MIC_SAMPLE_RATE 16000

/* Recordings are clipped here rather than growing truly unbounded, but
   this is just a generous memory sanity bound now, not a workaround for
   anything content- or length-related: an exactly-30.000s clip that
   originally looked like a real length limit turned out to be an
   unrelated, purely intermittent race in sherpa-onnx-offline's
   --num-threads=4 decoding path (see transcribe.c) — the very same file,
   run again unchanged, transcribed correctly. Fixed at the source
   (--num-threads=2) instead of by constraining recording length. */
#define FB_MIC_MAX_RECORD_SECONDS 300

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
   than FB_MIC_MAX_RECORD_SECONDS are truncated rather than growing
   unbounded — compare *out_count against
   FB_MIC_SAMPLE_RATE*FB_MIC_MAX_RECORD_SECONDS to tell the caller their
   recording got cut off. */
const float *fb_mic_recorded_samples(size_t *out_count);

#endif /* FB_MIC_CAPTURE_H */
