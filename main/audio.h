#pragma once
/* plain I2S into the speaker amps (call after board_init: it owns the amp pin) */
void audio_init(void);
void audio_beep(int hz, int ms);
