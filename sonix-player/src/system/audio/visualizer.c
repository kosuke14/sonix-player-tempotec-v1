#include "visualizer.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

// The audio thread never waits for the UI. Only a short, downsampled window is
// copied; analysis runs on the UI thread, never in the real-time write path.
#define WINDOW 256
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool active;
static float samples[WINDOW];
static int count, stride_phase, sample_rate;
static struct timespec updated;

void visualizer_enable(bool enabled) {
    atomic_store(&active, enabled);
    if (!enabled) {
        pthread_mutex_lock(&lock);
        count = stride_phase = 0;
        updated.tv_sec = updated.tv_nsec = 0;
        pthread_mutex_unlock(&lock);
    }
}

void visualizer_feed(const void *pcm, int frames, int channels, int bits, int rate) {
    if (!atomic_load(&active) || !pcm || frames <= 0 || channels <= 0 || rate <= 0 ||
        (bits != 16 && bits != 24 && bits != 32) || pthread_mutex_trylock(&lock)) return;
    int stride = rate / 11025;
    if (stride < 1) stride = 1;
    int sr = rate / stride;
    if (sample_rate != sr) { count = stride_phase = 0; sample_rate = sr; }
    const unsigned char *p = pcm;
    int bytes = bits / 8;
    for (int i = 0; i < frames; i++) {
        if (stride_phase++ % stride != 0) continue;
        float sum = 0;
        for (int c = 0; c < channels; c++) {
            const unsigned char *s = p + ((size_t)i * channels + c) * bytes;
            int32_t v;
            if (bits == 16) { int16_t x; memcpy(&x, s, 2); v = x; sum += v / 32768.0f; }
            else if (bits == 24) { v = (int32_t)((uint32_t)s[0] | (uint32_t)s[1]<<8 | (uint32_t)s[2]<<16); if (v & 0x800000) v |= (int32_t)0xff000000; sum += v / 8388608.0f; }
            else { memcpy(&v, s, 4); sum += v / 2147483648.0f; }
        }
        samples[count++ % WINDOW] = sum / channels;
        if (count >= WINDOW * 2) count -= WINDOW;
    }
    stride_phase %= stride;
    clock_gettime(CLOCK_MONOTONIC, &updated);
    pthread_mutex_unlock(&lock);
}

void visualizer_levels(uint8_t levels[VISUALIZER_BANDS]) {
    float data[WINDOW]; int n, rate; struct timespec stamp, now;
    memset(levels, 0, VISUALIZER_BANDS);
    pthread_mutex_lock(&lock);
    n = count < WINDOW ? count : WINDOW; rate = sample_rate; stamp = updated;
    for (int i = 0; i < n; i++) data[i] = samples[(count - n + i) % WINDOW];
    pthread_mutex_unlock(&lock);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (n < WINDOW || !stamp.tv_sec || now.tv_sec - stamp.tv_sec > 1 || rate <= 0) return;
    // Apply the window once, not once per band.
    static float window[WINDOW];
    static bool window_ready;
    if (!window_ready) {
        for (int i = 0; i < WINDOW; i++) window[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (WINDOW - 1));
        window_ready = true;
    }
    for (int i = 0; i < n; i++) data[i] *= window[i];
    // Log-spaced centres, Hann-windowed Goertzel filters.
    static const float freq[VISUALIZER_BANDS] = {85, 150, 270, 480, 850, 1500, 2700, 4400};
    for (int b = 0; b < VISUALIZER_BANDS; b++) {
        if (freq[b] >= rate * 0.5f) continue;
        float coeff = 2.0f * cosf(6.2831853f * freq[b] / rate), a = 0, z = 0;
        for (int i = 0; i < n; i++) {
            float x = data[i];
            float v = x + coeff * a - z; z = a; a = v;
        }
        float magnitude = sqrtf(fmaxf(0, a*a + z*z - coeff*a*z)) / (WINDOW * 0.25f);
        float level = (log10f(magnitude + 0.00001f) + 3.0f) / 3.0f;
        if (level < 0) level = 0;
        if (level > 1) level = 1;
        levels[b] = (uint8_t)(level * 255);
    }
}
