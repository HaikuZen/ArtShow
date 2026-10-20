#pragma once
/* the card's picture index: an SQLite database on the card
 * (/sdcard/.artshow/index.db): every JPEG with its folder, size, date, size
 * in pixels once shown, and whether it can be shown at all. The slideshow
 * lists the pictures from it instead of walking the card each boot; a rescan
 * (the Settings page) walks the card and brings it up to date. Everything
 * here runs in the slideshow's task, but index_summary */
#include <stdbool.h>
#include <stddef.h>

/* open (or make) the database; false: none (no card, or it can't be
 * written): the slideshow then walks the card as before */
bool index_open(void);
bool index_is_open(void);
/* walk the card and bring the index up to date: new pictures in, gone ones
 * out, changed ones (size or date) tried again. The number indexed, or -1 */
int index_scan(void);
/* the pictures that can be shown, sorted by path (full paths, each
 * malloc'd in PSRAM, into files[0..max)); their number */
int index_list(char **files, int max);
/* walk the card without an index: each picture's full path to add() */
void index_walk(void (*add)(const char *path));
/* a picture that can't be shown (not a JPEG, progressive, too big...): kept
 * out of the list until it changes; why: for the log and the database */
void index_bad(const char *path, const char *why);
/* shown fine: its size, the first time */
void index_good(const char *path, int w, int h);
/* a picture put on the card meanwhile (Artwork of the Day's Save) */
void index_add(const char *path);
/* "87 pictures (2 can't be shown), scanned Tue 20 Oct 17:40": any task */
void index_summary(char *buf, size_t n);
