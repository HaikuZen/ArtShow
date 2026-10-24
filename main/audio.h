#pragma once
/* plain I2S into the speaker amps (call after board_init: it owns the amp pin) */
void audio_init(void);
void audio_beep(int hz, int ms);

#include <stdbool.h>
#include <stdint.h>
#define AUDIO_RATE 16000
/* the speaker has one player at a time (the Generative page's music, the
 * slideshow's radio): audio_open waits for the other's audio_close, sets the
 * I2S rate (hz) and turns the amps on; audio_close turns them off */
void audio_open(int hz);
void audio_close(void);
/* the Generative page's music: audio_open(AUDIO_RATE), or audio_close */
void audio_stream(bool on);
/* mono samples out, each to both slots; blocks while the DMA is full (it
 * paces the writer). Between writes the DMA sends silence (auto clear) */
void audio_write(const int16_t *mono, int n);
/* stereo frames out (left, right, left ...), as audio_write */
void audio_write_stereo(const int16_t *lr, int frames);
