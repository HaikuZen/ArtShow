#pragma once
/* the favourite albums of a Deezer user, from Deezer's public API (no key: a
 * public profile's favourites; the user's ID in config.json, "deezer" >
 * "user_id"). The list in an SQLite cache (/sdcard/.artshow/albums.db) and
 * each cover on the card (/sdcard/.artshow/covers/<id>.jpg and <id>_m.jpg),
 * so the Album Wall works offline after a sync. Call from one task (the
 * page's worker); the SQLite calls take index.c's lock */
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    long long id;
    char title[128], artist[96];
    int year, tracks;
} album_t;

/* config.json's Deezer user ID; call after the card mount. false: none set */
bool albums_config(void);
const char *albums_user(void);
/* the favourites list from Deezer into the cache (3 calls for 297: ~10 s).
 * retry_misses (the Sync button): Wikipedia's "nothing" forgotten too. The
 * albums' number, or -1 and err. progress (may be NULL) hears each step */
int albums_sync_list(void (*progress)(const char *what), bool retry_misses, char *err, size_t n);
/* the covers not on the card yet ("Covers: 120 of 297"); stop (may be NULL)
 * is asked between albums: true breaks off. The number still missing */
int albums_sync_covers(void (*progress)(const char *what), bool (*stop)(void));
/* the cached albums, in the favourites' order */
int albums_count(void);
int albums_get(int offset, int limit, album_t *out);
/* a cover's file (big: 1000 px, else 250 px); false: not on the card yet */
bool albums_cover(long long id, bool big, char *path, size_t n);
/* Deezer has no cover for it (its address leads to Deezer's blank one) */
bool albums_no_cover(long long id);
/* an album's label, genres and tracks ("1. Title  5:30\n..."), from Deezer
 * (a call each time: the Spotlight's album) */
bool albums_details(long long id, char *label, size_t nl, char *genres, size_t ng, char *tracks, size_t nt);
/* what Wikipedia says about an album (its intro, its Personnel section, its
 * Reception section; "" where it has none), cached once looked up: a found
 * one for good, "nothing" for 30 days (and till a list sync) */
typedef struct {
    char page[128];
    char about[1500], credits[1500], reception[2500];
} album_wiki_t;
/* from the cache, else Wikipedia (English, then CONFIG_ARTSHOW_WIKI_LANG):
 * a few calls, a search only as the last resort (it's rate-limited). false:
 * no answer this time (429, no Wi-Fi, an error: anything but "no such
 * page"): not cached, asked again the next time it's shown */
bool albums_wiki(const album_t *a, album_wiki_t *w);

/* the cache closed (the page hidden): an open SQLite file and its journal
 * hold a 4 KB cache each in internal RAM; the next call opens it again */
void albums_close(void);
/* "297 albums, synced Wed 21 Oct 09:12" */
void albums_summary(char *buf, size_t n);
