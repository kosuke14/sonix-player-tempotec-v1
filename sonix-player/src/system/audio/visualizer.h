#ifndef SONIX_VISUALIZER_H
#define SONIX_VISUALIZER_H
#include <stdbool.h>
#include <stdint.h>
#define VISUALIZER_BANDS 8
void visualizer_enable(bool enabled);
void visualizer_feed(const void *pcm, int frames, int channels, int bits, int rate);
void visualizer_levels(uint8_t levels[VISUALIZER_BANDS]);
#endif
