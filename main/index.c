/* index.c - the picture index in SQLite. One table:
 *   pictures(path PRIMARY KEY  -- under the card: "DailyArt Pictures/x.jpg"
 *            folder            -- the top folder, "" at the top
 *            artist, title     -- from the file name, "Artist - Title.jpg"
 *            fav               -- a favourite (the Gallery's star): a copy of
 *                                 favourites.txt, see below
 *            size, mtime       -- a rescan tries a picture again if they change
 *            width, height     -- once shown
 *            status            -- 0 not tried, 1 shown, 2 can't be shown (note: why)
 *            added, seen)      -- seen: the rescan's mark
 * thumbs(path, w, h, jpeg): a small JPEG of each picture shown (the Gallery),
 * and meta(key, value) for the schema version and the last scan. A recursive
 * mutex makes each public call whole: the slideshow's task and the Gallery's
 * use the one connection. SQLite's
 * memory comes from PSRAM. Writes: a rescan (one transaction), a picture's
 * first showing, a failure: not each picture shown (the card's wear). */
#include "index.h"
#include "sqlite3.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MOUNT     "/sdcard"
#define DIR_      MOUNT "/.artshow"   /* a dot folder: the slideshow's walk skips it */
#define DB_PATH   DIR_ "/index.db"
/* the favourites: the only thing here a rescan can't find again, so not only
   in the database (made anew when it can't be read, or its layout changes):
   one path per line, the truth; the fav column copies it after each scan */
#define FAV_PATH  DIR_ "/favourites.txt"
#define SCHEMA    2   /* another: the database is made anew (it holds no favourites yet at 1) */
#define MAX_DEPTH 4

static const char *TAG = "index";

static sqlite3 *s_db;
static SemaphoreHandle_t s_lock;          /* the connection, one call at a time */
static volatile uint32_t s_thumbs_version;
static portMUX_TYPE s_sum_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_summary[96] = "No index";

/* ---- SQLite's memory: PSRAM, with the size in front (xSize wants it) ---- */

static void *m_malloc(int n) {
    int64_t *p = heap_caps_malloc(n + 8, MALLOC_CAP_SPIRAM);
    if (!p) return NULL;
    p[0] = n;
    return p + 1;
}
static void m_free(void *p) {
    if (p) heap_caps_free((int64_t *)p - 1);
}
static void *m_realloc(void *p, int n) {
    if (!p) return m_malloc(n);
    int64_t *q = heap_caps_realloc((int64_t *)p - 1, n + 8, MALLOC_CAP_SPIRAM);
    if (!q) return NULL;
    q[0] = n;
    return q + 1;
}
static int m_size(void *p) { return p ? (int)((int64_t *)p)[-1] : 0; }
static int m_roundup(int n) { return (n + 7) & ~7; }
static int m_init(void *a) { return SQLITE_OK; }
static void m_shutdown(void *a) {}

/* ---- helpers ---- */

static bool exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(s_db, sql, NULL, NULL, &err) == SQLITE_OK) return true;
    ESP_LOGW(TAG, "%s: %s", sql, err ? err : "?");
    sqlite3_free(err);
    return false;
}

static sqlite3_stmt *prepare(const char *sql) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) ESP_LOGW(TAG, "%s: %s", sql, sqlite3_errmsg(s_db));
    return st;
}

static int count(const char *sql) {
    sqlite3_stmt *st = prepare(sql);
    int n = st && sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

/* "/sdcard/DailyArt Pictures/x.jpg" -> "DailyArt Pictures/x.jpg" */
static const char *rel(const char *path) {
    size_t m = strlen(MOUNT);
    return !strncmp(path, MOUNT, m) && path[m] == '/' ? path + m + 1 : path;
}

static void folder_of(const char *relpath, char *out, size_t n) {
    const char *slash = strchr(relpath, '/');
    snprintf(out, n, "%.*s", slash ? (int)(slash - relpath) : 0, relpath);
}

/* "DailyArt Pictures/Paul Signac - Il pino.jpg": the artist and the title
   (DailyArt writes ':' as '_') */
static void split_name(const char *relpath, char *artist, size_t na, char *title, size_t nt) {
    const char *base = strrchr(relpath, '/');
    base = base ? base + 1 : relpath;
    char name[256];
    snprintf(name, sizeof name, "%s", base);
    char *dot = strrchr(name, '.');
    if (dot) *dot = 0;
    for (char *p = name; *p; p++) if (*p == '_') *p = p[1] == ' ' ? ':' : ' ';
    char *sep = strstr(name, " - ");
    if (sep) *sep = 0;
    snprintf(artist, na, "%s", sep ? name : "");
    snprintf(title, nt, "%s", sep ? sep + 3 : name);
}

/* the summary for the Settings page, from the database */
static void update_summary(void) {
    char buf[96], when[32] = "never";
    int all = count("SELECT count(*) FROM pictures"), bad = count("SELECT count(*) FROM pictures WHERE status = 2");
    sqlite3_stmt *st = prepare("SELECT value FROM meta WHERE key = 'scanned'");
    if (st && sqlite3_step(st) == SQLITE_ROW) {
        time_t t = sqlite3_column_int64(st, 0);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, t > 1600000000 ? "%a %e %b %H:%M" : "before the clock was set", &tm);
    }
    sqlite3_finalize(st);
    if (bad > 0) snprintf(buf, sizeof buf, "%d pictures (%d can't be shown), scanned %s", all - bad, bad, when);
    else snprintf(buf, sizeof buf, "%d pictures, scanned %s", all, when);
    taskENTER_CRITICAL(&s_sum_lock);
    strcpy(s_summary, buf);
    taskEXIT_CRITICAL(&s_sum_lock);
}

static void set_summary(const char *txt) {
    taskENTER_CRITICAL(&s_sum_lock);
    snprintf(s_summary, sizeof s_summary, "%s", txt);
    taskEXIT_CRITICAL(&s_sum_lock);
}

void index_summary(char *buf, size_t n) {
    taskENTER_CRITICAL(&s_sum_lock);
    snprintf(buf, n, "%s", s_summary);
    taskEXIT_CRITICAL(&s_sum_lock);
}

/* ---- the database ---- */

/* open, and make the tables; false: no good (s_db closed) */
static bool open_db(void) {
    if (sqlite3_open(DB_PATH, &s_db) != SQLITE_OK) {
        ESP_LOGW(TAG, "%s: %s", DB_PATH, s_db ? sqlite3_errmsg(s_db) : "no memory");
        sqlite3_close(s_db);
        s_db = NULL;
        return false;
    }
    /* TRUNCATE: the journal file stays (no directory writes each time).
       FULL: the database file is synced at each commit too. On FAT a file's
       size and clusters reach the card only when it's synced or closed, and
       the board is reset, never closed: with NORMAL the next boot read the
       database as "disk I/O error" */
    exec("PRAGMA journal_mode = TRUNCATE");
    exec("PRAGMA synchronous = FULL");
    bool ok = exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value)") &&
              exec("CREATE TABLE IF NOT EXISTS pictures("
                   " path TEXT PRIMARY KEY, folder TEXT NOT NULL, artist TEXT, title TEXT, size INTEGER, mtime INTEGER,"
                   " width INTEGER DEFAULT 0, height INTEGER DEFAULT 0, status INTEGER DEFAULT 0, note TEXT,"
                   " fav INTEGER DEFAULT 0, added INTEGER, seen INTEGER DEFAULT 1)") &&
              exec("CREATE TABLE IF NOT EXISTS thumbs(path TEXT PRIMARY KEY, w INTEGER, h INTEGER, jpeg BLOB)") &&
              exec("INSERT OR IGNORE INTO meta VALUES('schema', 2)");
    if (!ok || count("SELECT value FROM meta WHERE key = 'schema'") != SCHEMA) {
        sqlite3_close(s_db);
        s_db = NULL;
        return false;
    }
    return true;
}

static bool open_l(void) {
    if (mkdir(DIR_, 0777) && errno != EEXIST) {
        ESP_LOGW(TAG, "can't make %s", DIR_);
        set_summary("No index: the card can't be written");
        return false;
    }
    if (!open_db()) {   /* damaged, or another layout: a new one (it's only an index) */
        ESP_LOGW(TAG, "%s is no good: a new one", DB_PATH);
        remove(DB_PATH);
        remove(DB_PATH "-journal");
        if (!open_db()) {
            set_summary("No index: the database can't be made");
            return false;
        }
    }
    update_summary();
    ESP_LOGI(TAG, "%s: %s", DB_PATH, s_summary);
    return true;
}

bool index_is_open(void) { return s_db; }

/* ---- the walk ---- */

static bool is_jpeg(const char *name) {
    const char *e = strrchr(name, '.');
    return e && (!strcasecmp(e, ".jpg") || !strcasecmp(e, ".jpeg"));
}

typedef void (*found_fn)(const char *path, const struct stat *st, void *ctx);

static void walk(const char *dir, int depth, bool want_stat, found_fn found, void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return;
    char path[512];
    struct dirent *e;
    while ((e = readdir(d))) {
        /* dot files: macOS leaves a "._name.jpg" next to every picture; ours is .artshow */
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "System Volume Information")) continue;
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) continue;
        if (e->d_type == DT_DIR) {
            if (depth < MAX_DEPTH) walk(path, depth + 1, want_stat, found, ctx);
        } else if (is_jpeg(e->d_name)) {
            struct stat st = { 0 };
            if (want_stat) stat(path, &st);
            found(path, &st, ctx);
        }
    }
    closedir(d);
}

static void found_plain(const char *path, const struct stat *st, void *ctx) { ((void (*)(const char *))ctx)(path); }

void index_walk(void (*add)(const char *path)) { walk(MOUNT, 0, false, found_plain, (void *)add); }

static void upsert(const char *path, const struct stat *st, void *ctx) {
    sqlite3_stmt *ins = ctx;
    char folder[128];
    folder_of(rel(path), folder, sizeof folder);
    sqlite3_bind_text(ins, 1, rel(path), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, folder, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 3, st->st_size);
    sqlite3_bind_int64(ins, 4, st->st_mtime);
    sqlite3_bind_int64(ins, 5, time(NULL));
    char artist[96], title[160];
    split_name(rel(path), artist, sizeof artist, title, sizeof title);
    sqlite3_bind_text(ins, 6, artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 7, title, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(ins) != SQLITE_DONE) ESP_LOGW(TAG, "%s: %s", path, sqlite3_errmsg(s_db));
    sqlite3_reset(ins);
}

static int scan_l(void) {
    if (!s_db) return -1;
    set_summary("Scanning the card...");
    int64_t t0 = esp_log_timestamp();
    int before = count("SELECT count(*) FROM pictures");
    exec("BEGIN");
    exec("UPDATE pictures SET seen = 0");
    /* a changed picture (size or date) gets another try */
    sqlite3_stmt *ins = prepare(
        "INSERT INTO pictures(path, folder, size, mtime, added, artist, title) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)"
        " ON CONFLICT(path) DO UPDATE SET seen = 1,"
        "  status = CASE WHEN size IS NOT excluded.size OR mtime IS NOT excluded.mtime THEN 0 ELSE status END,"
        "  width = CASE WHEN size IS NOT excluded.size OR mtime IS NOT excluded.mtime THEN 0 ELSE width END,"
        "  size = excluded.size, mtime = excluded.mtime");
    if (ins) walk(MOUNT, 0, true, upsert, ins);
    sqlite3_finalize(ins);
    exec("DELETE FROM pictures WHERE seen = 0");
    int gone = sqlite3_changes(s_db);
    exec("DELETE FROM thumbs WHERE path NOT IN (SELECT path FROM pictures WHERE width > 0)");   /* gone, or changed */
    char sql[80];
    snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO meta VALUES('scanned', %lld)", (long long)time(NULL));
    exec(sql);
    bool ok = exec("COMMIT");
    if (!ok) exec("ROLLBACK");
    int now = count("SELECT count(*) FROM pictures");
    update_summary();
    ESP_LOGI(TAG, "scanned: %d pictures, %d new, %d gone, in %d ms", now, now - before + gone, gone,
             (int)(esp_log_timestamp() - t0));
    return ok ? now : -1;
}

static int list_l(char **files, int max) {
    if (!s_db) return 0;
    sqlite3_stmt *st = prepare("SELECT path FROM pictures WHERE status != 2 ORDER BY path");
    int n = 0;
    while (st && n < max && sqlite3_step(st) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(st, 0);
        char *full = heap_caps_malloc(strlen(MOUNT) + strlen(p) + 2, MALLOC_CAP_SPIRAM);
        if (!full) break;
        sprintf(full, "%s/%s", MOUNT, p);
        files[n++] = full;
    }
    sqlite3_finalize(st);
    return n;
}

static void set_status(const char *sql, const char *path, int a, int b, const char *note) {
    if (!s_db) return;
    sqlite3_stmt *st = prepare(sql);
    if (!st) return;
    sqlite3_bind_text(st, 1, rel(path), -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, a);
    sqlite3_bind_int(st, 3, b);
    if (note) sqlite3_bind_text(st, 4, note, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) ESP_LOGW(TAG, "%s: %s", path, sqlite3_errmsg(s_db));
    sqlite3_finalize(st);
}

static void bad_l(const char *path, const char *why) {
    set_status("UPDATE pictures SET status = 2, note = ?4 WHERE path = ?1 AND ?2 = ?3", path, 0, 0, why);
    update_summary();
    ESP_LOGI(TAG, "%s: can't be shown (%s): left out until it changes", rel(path), why);
}

static void good_l(const char *path, int w, int h) {
    /* only the first time: no write for a picture already known */
    set_status("UPDATE pictures SET status = 1, width = ?2, height = ?3 WHERE path = ?1 AND status = 0", path, w, h, NULL);
}

static void add_l(const char *path) {
    if (!s_db) return;
    struct stat st = { 0 };
    stat(path, &st);
    sqlite3_stmt *ins = prepare("INSERT OR IGNORE INTO pictures(path, folder, size, mtime, added, artist, title)"
                                " VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
    if (ins) upsert(path, &st, ins);
    sqlite3_finalize(ins);
    update_summary();
}

/* ---- favourites.txt ---- */

/* the file whole (malloc'd, "" if none: free it) */
static char *fav_read(void) {
    char *buf = NULL;
    FILE *f = fopen(FAV_PATH, "rb");
    long n = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    buf = heap_caps_calloc(1, n > 0 ? n + 1 : 1, MALLOC_CAP_SPIRAM);
    if (f && buf && n > 0 && fread(buf, 1, n, f) != (size_t)n) buf[0] = 0;
    if (f) fclose(f);
    return buf;
}

/* add or take out one line, and write it back */
static void fav_write(const char *relpath, bool fav) {
    char *buf = fav_read();
    if (!buf) return;
    FILE *f = fopen(FAV_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "can't write %s", FAV_PATH);
        free(buf);
        return;
    }
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
        if (*line && strcmp(line, relpath)) fprintf(f, "%s\n", line);
    if (fav) fprintf(f, "%s\n", relpath);
    fflush(f);
    fsync(fileno(f));   /* FAT: the size reaches the card only now (the board is reset, never closed) */
    fclose(f);
    free(buf);
}

/* the file into the fav column: after a scan (a new database too), an add */
static void fav_apply(void) {
    char *buf = fav_read();
    sqlite3_stmt *st = buf ? prepare("UPDATE pictures SET fav = 1 WHERE path = ?1") : NULL;
    for (char *line = buf && st ? strtok(buf, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        sqlite3_bind_text(st, 1, line, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    free(buf);
}

/* ---- the Gallery's ---- */

/* the WHERE of a filter; ?1 is the artist */
static const char *where(index_filter_t f) {
    switch (f) {
    case INDEX_FAVOURITES: return "p.status != 2 AND p.fav = 1";
    case INDEX_ARTIST: return "p.status != 2 AND p.artist = ?1";
    case INDEX_BAD: return "p.status = 2";
    default: return "p.status != 2";
    }
}

static int page_l(index_filter_t f, const char *artist, int offset, int limit, index_item_t *out, int *total) {
    char sql[400];
    *total = 0;
    if (!s_db) return 0;
    snprintf(sql, sizeof sql, "SELECT count(*) FROM pictures p WHERE %s", where(f));
    sqlite3_stmt *st = prepare(sql);
    if (st && f == INDEX_ARTIST) sqlite3_bind_text(st, 1, artist, -1, SQLITE_STATIC);
    if (st && sqlite3_step(st) == SQLITE_ROW) *total = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    snprintf(sql, sizeof sql,
             "SELECT p.path, p.artist, p.title, p.fav, p.status, p.note, t.path IS NOT NULL"
             " FROM pictures p LEFT JOIN thumbs t ON t.path = p.path WHERE %s"
             " ORDER BY p.artist COLLATE NOCASE, p.title COLLATE NOCASE LIMIT ?2 OFFSET ?3", where(f));
    st = prepare(sql);
    if (!st) return 0;
    if (f == INDEX_ARTIST) sqlite3_bind_text(st, 1, artist, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, limit);
    sqlite3_bind_int(st, 3, offset);
    int n = 0;
    while (n < limit && sqlite3_step(st) == SQLITE_ROW) {
        index_item_t *it = &out[n++];
        snprintf(it->path, sizeof it->path, "%s/%s", MOUNT, (const char *)sqlite3_column_text(st, 0));
        snprintf(it->artist, sizeof it->artist, "%s", (const char *)sqlite3_column_text(st, 1) ?: "");
        snprintf(it->title, sizeof it->title, "%s", (const char *)sqlite3_column_text(st, 2) ?: "");
        it->fav = sqlite3_column_int(st, 3);
        it->bad = sqlite3_column_int(st, 4) == 2;
        snprintf(it->note, sizeof it->note, "%s", (const char *)sqlite3_column_text(st, 5) ?: "");
        it->thumb = sqlite3_column_int(st, 6);
    }
    sqlite3_finalize(st);
    return n;
}

static int artists_l(char (*names)[96], int *counts, int offset, int max, int *total) {
    *total = 0;
    if (!s_db) return 0;
    *total = count("SELECT count(DISTINCT artist) FROM pictures WHERE status != 2");
    sqlite3_stmt *st = prepare("SELECT artist, count(*) FROM pictures WHERE status != 2"
                               " GROUP BY artist ORDER BY artist COLLATE NOCASE LIMIT ?1 OFFSET ?2");
    if (!st) return 0;
    sqlite3_bind_int(st, 1, max);
    sqlite3_bind_int(st, 2, offset);
    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(names[n], 96, "%s", (const char *)sqlite3_column_text(st, 0) ?: "");
        counts[n++] = sqlite3_column_int(st, 1);
    }
    sqlite3_finalize(st);
    return n;
}

static bool thumb_get_l(const char *path, uint8_t **jpeg, size_t *len) {
    *jpeg = NULL;
    if (!s_db) return false;
    sqlite3_stmt *st = prepare("SELECT jpeg FROM thumbs WHERE path = ?1");
    if (!st) return false;
    sqlite3_bind_text(st, 1, rel(path), -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        int n = sqlite3_column_bytes(st, 0);
        const void *b = sqlite3_column_blob(st, 0);
        if (n > 0 && (*jpeg = heap_caps_malloc(n, MALLOC_CAP_SPIRAM))) {
            memcpy(*jpeg, b, n);
            *len = n;
        }
    }
    sqlite3_finalize(st);
    return *jpeg;
}

static void thumb_put_l(const char *path, int w, int h, const uint8_t *jpeg, size_t len) {
    if (!s_db) return;
    sqlite3_stmt *st = prepare("INSERT OR REPLACE INTO thumbs VALUES(?1, ?2, ?3, ?4)");
    if (!st) return;
    sqlite3_bind_text(st, 1, rel(path), -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, w);
    sqlite3_bind_int(st, 3, h);
    sqlite3_bind_blob(st, 4, jpeg, len, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_DONE) ESP_LOGW(TAG, "%s: %s", path, sqlite3_errmsg(s_db));
    else s_thumbs_version++;
    sqlite3_finalize(st);
}

static bool random_thumb_l(char *path, size_t n, uint8_t **jpeg, size_t *len) {
    *jpeg = NULL;
    if (!s_db) return false;
    sqlite3_stmt *st = prepare("SELECT path, jpeg FROM thumbs ORDER BY random() LIMIT 1");
    if (st && sqlite3_step(st) == SQLITE_ROW) {
        snprintf(path, n, "%s/%s", MOUNT, (const char *)sqlite3_column_text(st, 0));
        int b = sqlite3_column_bytes(st, 1);
        if (b > 0 && (*jpeg = heap_caps_malloc(b, MALLOC_CAP_SPIRAM))) {
            memcpy(*jpeg, sqlite3_column_blob(st, 1), b);
            *len = b;
        }
    }
    sqlite3_finalize(st);
    return *jpeg;
}

static int path_query(const char *sql, const char *path) {
    if (!s_db) return 0;
    sqlite3_stmt *st = prepare(sql);
    if (!st) return 0;
    sqlite3_bind_text(st, 1, rel(path), -1, SQLITE_STATIC);
    int r = sqlite3_step(st) == SQLITE_ROW ? sqlite3_column_int(st, 0) : 0;
    sqlite3_finalize(st);
    return r;
}

static bool next_unthumbed_l(const char *after, char *path, size_t n) {
    if (!s_db) return false;
    sqlite3_stmt *st = prepare("SELECT path FROM pictures WHERE status != 2 AND path > ?1"
                               " AND path NOT IN (SELECT path FROM thumbs) ORDER BY path LIMIT 1");
    if (!st) return false;
    sqlite3_bind_text(st, 1, after[0] ? rel(after) : "", -1, SQLITE_STATIC);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) snprintf(path, n, "%s/%s", MOUNT, (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return found;
}

static void set_fav_l(const char *path, bool fav) {
    fav_write(rel(path), fav);
    set_status("UPDATE pictures SET fav = ?2 WHERE path = ?1 AND ?3 = 0", path, fav, 0, NULL);
}

/* ---- the public calls: each under the lock ---- */


#define LOCK()   (s_lock ? xSemaphoreTakeRecursive(s_lock, portMAX_DELAY) : 0)
#define UNLOCK() (s_lock ? xSemaphoreGiveRecursive(s_lock) : 0)

/* SQLite's setup (its memory from PSRAM) and the lock, once: whichever
   database opens first (this, or the notes') */
static void sqlite_setup(void) {
    static bool done;
    if (done) return;
    static const sqlite3_mem_methods mem = { m_malloc, m_free, m_realloc, m_size, m_roundup, m_init, m_shutdown, NULL };
    sqlite3_config(SQLITE_CONFIG_MALLOC, &mem);   /* before SQLite's first use */
    s_lock = xSemaphoreCreateRecursiveMutex();
    done = true;
}

void index_sqlite_lock(void) {
    sqlite_setup();
    LOCK();
}

void index_sqlite_unlock(void) { UNLOCK(); }

bool index_open(void) {
    sqlite_setup();
    LOCK();
    bool r = open_l();
    UNLOCK();
    return r;
}
int index_scan(void) {
    LOCK();
    int r = scan_l();
    if (r >= 0) fav_apply();
    UNLOCK();
    return r;
}
int index_list(char **files, int max) {
    LOCK();
    int r = list_l(files, max);
    UNLOCK();
    return r;
}
void index_bad(const char *path, const char *why) {
    LOCK();
    bad_l(path, why);
    UNLOCK();
}
void index_good(const char *path, int w, int h) {
    LOCK();
    good_l(path, w, h);
    UNLOCK();
}
void index_add(const char *path) {
    LOCK();
    add_l(path);
    fav_apply();
    UNLOCK();
}
int index_page(index_filter_t f, const char *artist, int offset, int limit, index_item_t *out, int *total) {
    LOCK();
    int r = page_l(f, artist, offset, limit, out, total);
    UNLOCK();
    return r;
}
int index_artists(char (*names)[96], int *counts, int offset, int max, int *total) {
    LOCK();
    int r = artists_l(names, counts, offset, max, total);
    UNLOCK();
    return r;
}
bool index_thumb_get(const char *path, uint8_t **jpeg, size_t *len) {
    LOCK();
    bool r = thumb_get_l(path, jpeg, len);
    UNLOCK();
    return r;
}
void index_thumb_put(const char *path, int w, int h, const uint8_t *jpeg, size_t len) {
    LOCK();
    thumb_put_l(path, w, h, jpeg, len);
    UNLOCK();
}
bool index_has_thumb(const char *path) {
    LOCK();
    bool r = path_query("SELECT count(*) FROM thumbs WHERE path = ?1", path);
    UNLOCK();
    return r;
}
bool index_next_unthumbed(const char *after, char *path, size_t n) {
    LOCK();
    bool r = next_unthumbed_l(after, path, n);
    UNLOCK();
    return r;
}
void index_set_fav(const char *path, bool fav) {
    LOCK();
    set_fav_l(path, fav);
    UNLOCK();
}
bool index_is_fav(const char *path) {
    LOCK();
    bool r = path_query("SELECT fav FROM pictures WHERE path = ?1", path);
    UNLOCK();
    return r;
}
bool index_random_thumb(char *path, size_t n, uint8_t **jpeg, size_t *len) {
    LOCK();
    bool r = random_thumb_l(path, n, jpeg, len);
    UNLOCK();
    return r;
}
uint32_t index_thumbs_version(void) { return s_thumbs_version; }
