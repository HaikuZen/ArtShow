#pragma once
/* the card's picture index: an SQLite database on the card
 * (/sdcard/.artshow/index.db): every JPEG with its folder, size, date, size
 * in pixels once shown, and whether it can be shown at all. The slideshow
 * lists the pictures from it instead of walking the card each boot; a rescan
 * (the Settings page) walks the card and brings it up to date. Thumbnails and
 * favourites for the Gallery. Any task: a mutex makes each call whole (a
 * rescan holds it ~1 s) */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { INDEX_ALL, INDEX_FAVOURITES, INDEX_ARTIST, INDEX_BAD } index_filter_t;

typedef struct {
    char path[256];         /* full: "/sdcard/..." */
    char artist[96], title[160];
    bool fav, bad, thumb;   /* bad: it can't be shown (note: why) */
    char note[64];
} index_item_t;

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

/* the Gallery: a page of a filter's pictures (by artist, then title), and
 * how many in all */
int index_page(index_filter_t f, const char *artist, int offset, int limit, index_item_t *out, int *total);
/* the artists, with how many pictures each, a page of them */
int index_artists(char (*names)[96], int *counts, int offset, int max, int *total);
/* a picture's thumbnail (a JPEG, malloc'd: free it); false: none yet */
bool index_thumb_get(const char *path, uint8_t **jpeg, size_t *len);
void index_thumb_put(const char *path, int w, int h, const uint8_t *jpeg, size_t len);
bool index_has_thumb(const char *path);
/* the first picture after `after` (by path; "" from the start) with no
 * thumbnail yet; false: none */
bool index_next_unthumbed(const char *after, char *path, size_t n);
/* favourites: kept in the index (a rescan keeps them) */
void index_set_fav(const char *path, bool fav);
bool index_is_fav(const char *path);
/* SQLite is built for one thread (SQLITE_THREADSAFE=0): every SQLite call,
 * this index's or another database's (notes.c), holds this lock */
void index_sqlite_lock(void);
void index_sqlite_unlock(void);
/* a random picture's thumbnail (its path; the JPEG malloc'd: free it); false:
 * no thumbnails yet. The Generative page's colours */
bool index_random_thumb(char *path, size_t n, uint8_t **jpeg, size_t *len);
/* goes up each time a thumbnail is stored: the Gallery redraws */
uint32_t index_thumbs_version(void);
