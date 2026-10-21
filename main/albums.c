/* albums.c - Deezer's /user/<id>/albums (100 a page, the most recently
 * favourited first) into albums(id, pos, title, artist, year, tracks, small,
 * big, seen) in its own database: a cache, made anew if it can't be read
 * (nothing in it is the user's own). The covers: Deezer's 1000 px and 250 px
 * baseline JPEGs, ~100 KB and ~20 KB, downloaded once. */
#include "albums.h"
#include "index.h"
#include "net.h"
#include "sqlite3.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DIR_     "/sdcard/.artshow"
#define COVERS   DIR_ "/covers"
#define DB_PATH  DIR_ "/albums.db"
#define API      "https://api.deezer.com"

static const char *TAG = "albums";

static char s_user[24];
static sqlite3 *s_db;

/* ---- config.json ---- */

bool albums_config(void) {
    FILE *f = fopen("/sdcard/config.json", "rb");
    if (!f) return false;
    char *text = heap_caps_calloc(1, 16 * 1024, MALLOC_CAP_SPIRAM);   /* not 16 KB of internal RAM for good */
    if (!text) {
        fclose(f);
        return false;
    }
    fread(text, 1, 16 * 1024 - 1, f);
    fclose(f);
    cJSON *j = cJSON_Parse(text);
    free(text);
    const cJSON *id = json_at(j, "deezer.user_id");
    if (cJSON_IsString(id)) snprintf(s_user, sizeof s_user, "%s", id->valuestring);
    else if (cJSON_IsNumber(id)) snprintf(s_user, sizeof s_user, "%.0f", id->valuedouble);
    cJSON_Delete(j);
    for (char *p = s_user; *p; p++)   /* digits only: it goes in a URL */
        if (*p < '0' || *p > '9') s_user[0] = 0;
    if (s_user[0]) ESP_LOGI(TAG, "Deezer user %s", s_user);
    return s_user[0];
}

const char *albums_user(void) { return s_user; }

/* ---- the cache ---- */

static bool open_db(void) {
    if (s_db) return true;
    mkdir(DIR_, 0777);
    mkdir(COVERS, 0777);
    for (int tries = 0; tries < 2; tries++) {
        if (sqlite3_open(DB_PATH, &s_db) == SQLITE_OK &&
            sqlite3_exec(s_db, "PRAGMA journal_mode = TRUNCATE; PRAGMA synchronous = FULL;"
                               "CREATE TABLE IF NOT EXISTS albums(id INTEGER PRIMARY KEY, pos INTEGER, title TEXT,"
                               " artist TEXT, year INTEGER, tracks INTEGER, small TEXT, big TEXT, seen INTEGER);"
                               "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value)",
                         NULL, NULL, NULL) == SQLITE_OK)
            return true;
        sqlite3_close(s_db);   /* a cache: a new one */
        s_db = NULL;
        remove(DB_PATH);
        remove(DB_PATH "-journal");
    }
    ESP_LOGW(TAG, "%s can't be made", DB_PATH);
    return false;
}

static void exec(const char *sql) { sqlite3_exec(s_db, sql, NULL, NULL, NULL); }

void albums_close(void) {
    index_sqlite_lock();
    sqlite3_close(s_db);   /* NULL: nothing */
    s_db = NULL;
    index_sqlite_unlock();
}

int albums_count(void) {
    int n = 0;
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (open_db() && sqlite3_prepare_v2(s_db, "SELECT count(*) FROM albums", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    index_sqlite_unlock();
    return n;
}

int albums_get(int offset, int limit, album_t *out) {
    int n = 0;
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (open_db() && sqlite3_prepare_v2(s_db, "SELECT id, title, artist, year, tracks FROM albums ORDER BY pos LIMIT ?1 OFFSET ?2",
                                        -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, limit);
        sqlite3_bind_int(st, 2, offset);
        while (n < limit && sqlite3_step(st) == SQLITE_ROW) {
            album_t *a = &out[n++];
            a->id = sqlite3_column_int64(st, 0);
            snprintf(a->title, sizeof a->title, "%s", (const char *)sqlite3_column_text(st, 1) ?: "");
            snprintf(a->artist, sizeof a->artist, "%s", (const char *)sqlite3_column_text(st, 2) ?: "");
            a->year = sqlite3_column_int(st, 3);
            a->tracks = sqlite3_column_int(st, 4);
        }
    }
    sqlite3_finalize(st);
    index_sqlite_unlock();
    return n;
}

bool albums_cover(long long id, bool big, char *path, size_t n) {
    snprintf(path, n, COVERS "/%lld%s.jpg", id, big ? "" : "_m");
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

bool albums_no_cover(long long id) {
    char path[80];
    struct stat st;
    snprintf(path, sizeof path, COVERS "/%lld.none", id);
    return stat(path, &st) == 0;
}

void albums_summary(char *buf, size_t n) {
    int count = albums_count();
    char when[40] = "never";
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (open_db() && sqlite3_prepare_v2(s_db, "SELECT value FROM meta WHERE key = 'synced'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        time_t t = sqlite3_column_int64(st, 0);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, "%a %e %b %H:%M", &tm);
    }
    sqlite3_finalize(st);
    index_sqlite_unlock();
    snprintf(buf, n, "%d albums, synced %s", count, when);
}

/* ---- the sync ---- */

/* one page of the favourites into the cache; the number in it, or -1 */
static int sync_page(int index, int *total, char *err, size_t n) {
    char url[160];
    snprintf(url, sizeof url, API "/user/%s/albums?limit=100&index=%d", s_user, index);
    cJSON *j = net_get_json(url, "Deezer", err, n);
    if (!j) return -1;
    const char *e = json_str(j, "error.message");
    if (e) {
        snprintf(err, n, "Deezer: %s", e);
        cJSON_Delete(j);
        return -1;
    }
    *total = json_num(j, "total", 0);
    int got = 0;
    const cJSON *it;
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (open_db() && sqlite3_prepare_v2(s_db, "INSERT OR REPLACE INTO albums VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 1)", -1,
                                        &st, NULL) == SQLITE_OK) {
        exec("BEGIN");
        cJSON_ArrayForEach(it, json_at(j, "data")) {
            sqlite3_bind_int64(st, 1, (long long)json_num(it, "id", 0));
            sqlite3_bind_int(st, 2, index + got);
            sqlite3_bind_text(st, 3, json_str(it, "title") ?: "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 4, json_str(it, "artist.name") ?: "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 5, atoi(json_str(it, "release_date") ?: "0"));
            sqlite3_bind_int(st, 6, json_num(it, "nb_tracks", 0));
            sqlite3_bind_text(st, 7, json_str(it, "cover_medium") ?: "", -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 8, json_str(it, "cover_xl") ?: "", -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
            sqlite3_reset(st);
            got++;
        }
        exec("COMMIT");
    }
    sqlite3_finalize(st);
    index_sqlite_unlock();
    cJSON_Delete(j);
    return got;
}

/* the covers not on the card: (id, small url, big url) rows */
static int missing(long long *ids, char (*small)[160], char (*big)[160], int max) {
    int n = 0;
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (open_db() && sqlite3_prepare_v2(s_db, "SELECT id, small, big FROM albums ORDER BY pos", -1, &st, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW) {
            long long id = sqlite3_column_int64(st, 0);
            char p[80];
            if (albums_no_cover(id) || (albums_cover(id, false, p, sizeof p) && albums_cover(id, true, p, sizeof p))) continue;
            ids[n] = id;
            snprintf(small[n], 160, "%s", (const char *)sqlite3_column_text(st, 1) ?: "");
            snprintf(big[n], 160, "%s", (const char *)sqlite3_column_text(st, 2) ?: "");
            n++;
        }
    }
    sqlite3_finalize(st);
    index_sqlite_unlock();
    return n;
}

int albums_sync_list(void (*progress)(const char *what), char *err, size_t n) {
    if (!s_user[0]) {
        snprintf(err, n, "No Deezer user ID in config.json");
        return -1;
    }
    if (progress) progress("Getting your favourite albums from Deezer...");
    index_sqlite_lock();
    bool ok = open_db();
    if (ok) exec("UPDATE albums SET seen = 0");
    index_sqlite_unlock();
    if (!ok) {
        snprintf(err, n, "The album cache can't be made on the card");
        return -1;
    }
    int total = 0, done = 0;
    do {   /* the pages, till Deezer's total */
        int got = sync_page(done, &total, err, n);
        if (got < 0) return -1;
        if (!got) break;
        done += got;
    } while (done < total);
    index_sqlite_lock();
    exec("DELETE FROM albums WHERE seen = 0");   /* no longer a favourite */
    char sql[80];
    snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO meta VALUES('synced', %lld)", (long long)time(NULL));
    exec(sql);
    index_sqlite_unlock();
    ESP_LOGI(TAG, "%d favourite albums", done);
    return done;
}

int albums_sync_covers(void (*progress)(const char *what), bool (*stop)(void)) {
    /* in PSRAM: 128 KB of URLs */
    long long *ids = heap_caps_malloc(400 * sizeof *ids, MALLOC_CAP_SPIRAM);
    char (*small)[160] = heap_caps_malloc(400 * 160, MALLOC_CAP_SPIRAM), (*big)[160] = heap_caps_malloc(400 * 160, MALLOC_CAP_SPIRAM);
    int miss = ids && small && big ? missing(ids, small, big, 400) : 0, k = 0;
    for (; k < miss && !(stop && stop()); k++) {   /* both sizes, in the favourites' order */
        char msg[48], path[80], e[120];
        bool too_big;
        snprintf(msg, sizeof msg, "Covers: %d of %d", k + 1, miss);
        if (progress) progress(msg);
        e[0] = 0;
        bool none = !small[k][0] || !big[k][0];
        if (!none && !albums_cover(ids[k], false, path, sizeof path) &&
            !net_download(small[k], "Deezer's covers", path, 1024 * 1024, &too_big, NULL, e, sizeof e))
            none = strstr(e, "HTTP 3") || strstr(e, "HTTP 404");
        if (!none && !albums_cover(ids[k], true, path, sizeof path) &&
            !net_download(big[k], "Deezer's covers", path, 4 * 1024 * 1024, &too_big, NULL, e, sizeof e))
            none = strstr(e, "HTTP 3") || strstr(e, "HTTP 404");
        if (none) {   /* Deezer has none: it answers with its blank one's address. Not asked again */
            snprintf(path, sizeof path, COVERS "/%lld.none", ids[k]);
            FILE *f = fopen(path, "wb");
            if (f) fclose(f);
        }
    }
    free(ids);
    free(small);
    free(big);
    if (miss) ESP_LOGI(TAG, "covers: %d of %d downloaded", k, miss);
    return miss - k;
}

bool albums_details(long long id, char *label, size_t nl, char *genres, size_t ng, char *tracks, size_t nt) {
    char url[80], err[120];
    snprintf(url, sizeof url, API "/album/%lld", id);
    label[0] = genres[0] = tracks[0] = 0;
    cJSON *j = net_get_json(url, "Deezer", err, sizeof err);
    if (!j) return false;
    snprintf(label, nl, "%s", json_str(j, "label") ?: "");
    const cJSON *g;
    cJSON_ArrayForEach(g, json_at(j, "genres.data"))
        snprintf(genres + strlen(genres), ng - strlen(genres), "%s%s", genres[0] ? ", " : "", json_str(g, "name") ?: "");
    int k = 0;
    cJSON_ArrayForEach(g, json_at(j, "tracks.data")) {
        int d = json_num(g, "duration", 0);
        size_t l = strlen(tracks);
        snprintf(tracks + l, nt - l, "%d.  %s   %d:%02d\n", ++k, json_str(g, "title") ?: "", d / 60, d % 60);
    }
    cJSON_Delete(j);
    return true;
}
