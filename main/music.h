#pragma once
/* the Generative page's music: music_synth.c played on the speaker by a
 * task. Any task may call these */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void music_init(void);
/* a new piece's music (the picture's kind, seed and colours); desc: "E lydian, 72 bpm" */
void music_piece(uint32_t seed, int style, const uint32_t *pal, char *desc, size_t n);
/* on or off (it fades out in 0.3 s, then the amps go off) */
void music_play(bool on);
/* night mode's dark (dimmed or off): silent meanwhile, whatever music_play
 * says; a touch lights the screen and the music comes back */
void music_quiet(bool quiet);
