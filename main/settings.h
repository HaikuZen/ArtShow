#pragma once
/* the settings the Settings page changes, kept in NVS (artshow/settings and
 * artshow/skipdirs). The defaults, before the first change, come from
 * menuconfig > ArtShow slideshow. g_settings is written only in the LVGL task
 * (the page) and read anywhere: each field is one word, read when needed */
#include <stdbool.h>
#include <stdint.h>

typedef enum { NIGHT_OFF, NIGHT_SUN, NIGHT_HOURS } night_mode_t;

typedef struct {
    int32_t interval_s;     /* seconds per picture */
    int32_t fade_ms;        /* the transition */
    bool varied;            /* varied transitions, else the dissolve */
    bool shuffle;           /* else name order */
    bool fill;              /* crop to fill the screen, else whole with bars */
    int32_t night_mode;     /* night_mode_t */
    int32_t night_from;     /* minutes after midnight (NIGHT_HOURS) */
    int32_t night_to;
    int32_t night_level;    /* the backlight at night, %; 0: off */
    bool favourites_only;   /* the Gallery's stars; none starred: all */
} settings_t;

extern settings_t g_settings;

/* read them from NVS; call once, after nvs_flash_init */
void settings_load(void);
/* write them to NVS: once per change (a slider's release, not each step) */
void settings_save(void);

/* the slideshow's folders: by their name under the card's top ("" for the
 * pictures at the top itself). All play unless switched off here; any task */
bool settings_folder_on(const char *folder);
void settings_folder_set(const char *folder, bool on);   /* saved at once */
