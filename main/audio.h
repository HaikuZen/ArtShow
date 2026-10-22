#pragma once
/* plain I2S into the speaker amps (call after board_init: it owns the amp pin) */
void audio_init(void);
void audio_beep(int hz, int ms);

#include <stdbool.h>
#include <stdint.h>
#define AUDIO_RATE 16000
/* a stream (the Generative page's music): the amps on or off */
void audio_stream(bool on);
/* mono samples out, each to both slots; blocks while the DMA is full (it
 * paces the writer). Between writes the DMA sends silence (auto clear) */
void audio_write(const int16_t *mono, int n);
