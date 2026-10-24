#pragma once
/* the slideshow's radio: an internet station (MP3 or AAC over HTTP) on the
 * speaker, quietly, while the slideshow is on screen. The stations: a few
 * calm ones built in, or config.json's "radio": { "stations": [ { "name":
 * "...", "url": "..." } ] }. Any task may call these */
#include <stdbool.h>
#include <stddef.h>

#define RADIO_MAX_STATIONS 12

/* the stations from config.json on the card (card: it's mounted), else the
 * built-in ones; call once at boot */
void radio_config(bool card);
int radio_count(void);
const char *radio_name(int k);

/* play or stop (stopping fades out in 0.6 s; playing fades in over 4 s).
 * The station and volume are g_settings' radio_station and radio_volume,
 * read as it plays: radio_retune() after a new station */
void radio_play(bool on);
void radio_retune(void);
/* the stream, for the screen (Now Showing); false while it's off */
typedef struct {
    char station[48];
    char title[160];      /* the server's StreamTitle, "Artist - Title" mostly; "" if none */
    const char *codec;    /* "MP3", "AAC"; NULL until decoded */
    int kbps, rate, channels;
    char status[96];      /* as radio_status */
} radio_info_t;
bool radio_info(radio_info_t *out);

/* what it's doing, for the screen: "Drone Zone", "Tuning in to ...",
 * "No signal: ..."; false while it's off */
bool radio_status(char *buf, size_t n);
