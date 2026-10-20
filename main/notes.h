#pragma once
/* notes for days of the calendar (the clock page): one note a day, in an
 * SQLite database of its own on the card (/sdcard/.artshow/notes.db). Not the
 * picture index's: that one is made anew when it can't be read, and notes are
 * written, not found again. One that can't be read is set aside, not deleted.
 * Any task (index.c's SQLite lock) */
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    short year;
    signed char month, day;
} notes_date_t;

/* the note of that day into buf ("" none); false: none, or no database */
bool notes_get(int year, int month, int day, char *buf, size_t n);
/* set it; "" deletes it. false: it can't be saved (no card) */
bool notes_set(int year, int month, int day, const char *text);
/* the days with a note, up to max, in order; their number */
int notes_days(notes_date_t *out, int max);
