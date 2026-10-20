/* index.c - the picture index in SQLite. One table:
 *   pictures(path PRIMARY KEY  -- under the card: "DailyArt Pictures/x.jpg"
 *            folder            -- the top folder, "" at the top
 *            size, mtime       -- a rescan tries a picture again if they change
 *            width, height     -- once shown
 *            status            -- 0 not tried, 1 shown, 2 can't be shown (note: why)
 *            added, seen)      -- seen: the rescan's mark
 * and meta(key, value) for the schema version and the last scan. SQLite's
 * memory comes from PSRAM. Writes: a rescan (one transaction), a picture's
 * first showing, a failure: not each picture shown (the card's wear). */
#include "index.h"
#include "sqlite3.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MOUNT     "/sdcard"
#define DIR_      MOUNT "/.artshow"   /* a dot folder: the slideshow's walk skips it */
#define DB_PATH   DIR_ "/index.db"
#define SCHEMA    1
#define MAX_DEPTH 4

static const char *TAG = "index";

static sqlite3 *s_db;
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
                   " path TEXT PRIMARY KEY, folder TEXT NOT NULL, size INTEGER, mtime INTEGER,"
                   " width INTEGER DEFAULT 0, height INTEGER DEFAULT 0, status INTEGER DEFAULT 0, note TEXT,"
                   " added INTEGER, seen INTEGER DEFAULT 1)") &&
              exec("INSERT OR IGNORE INTO meta VALUES('schema', 1)");
    if (!ok || count("SELECT value FROM meta WHERE key = 'schema'") != SCHEMA) {
        sqlite3_close(s_db);
        s_db = NULL;
        return false;
    }
    return true;
}

bool index_open(void) {
    static const sqlite3_mem_methods mem = { m_malloc, m_free, m_realloc, m_size, m_roundup, m_init, m_shutdown, NULL };
    sqlite3_config(SQLITE_CONFIG_MALLOC, &mem);   /* before SQLite's first use */
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
    if (sqlite3_step(ins) != SQLITE_DONE) ESP_LOGW(TAG, "%s: %s", path, sqlite3_errmsg(s_db));
    sqlite3_reset(ins);
}

int index_scan(void) {
    if (!s_db) return -1;
    set_summary("Scanning the card...");
    int64_t t0 = esp_log_timestamp();
    int before = count("SELECT count(*) FROM pictures");
    exec("BEGIN");
    exec("UPDATE pictures SET seen = 0");
    /* a changed picture (size or date) gets another try */
    sqlite3_stmt *ins = prepare(
        "INSERT INTO pictures(path, folder, size, mtime, added) VALUES(?1, ?2, ?3, ?4, ?5)"
        " ON CONFLICT(path) DO UPDATE SET seen = 1,"
        "  status = CASE WHEN size IS NOT excluded.size OR mtime IS NOT excluded.mtime THEN 0 ELSE status END,"
        "  width = CASE WHEN size IS NOT excluded.size OR mtime IS NOT excluded.mtime THEN 0 ELSE width END,"
        "  size = excluded.size, mtime = excluded.mtime");
    if (ins) walk(MOUNT, 0, true, upsert, ins);
    sqlite3_finalize(ins);
    exec("DELETE FROM pictures WHERE seen = 0");
    int gone = sqlite3_changes(s_db);
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

int index_list(char **files, int max) {
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

void index_bad(const char *path, const char *why) {
    set_status("UPDATE pictures SET status = 2, note = ?4 WHERE path = ?1 AND ?2 = ?3", path, 0, 0, why);
    update_summary();
    ESP_LOGI(TAG, "%s: can't be shown (%s): left out until it changes", rel(path), why);
}

void index_good(const char *path, int w, int h) {
    /* only the first time: no write for a picture already known */
    set_status("UPDATE pictures SET status = 1, width = ?2, height = ?3 WHERE path = ?1 AND status = 0", path, w, h, NULL);
}

void index_add(const char *path) {
    if (!s_db) return;
    struct stat st = { 0 };
    stat(path, &st);
    sqlite3_stmt *ins = prepare("INSERT OR IGNORE INTO pictures(path, folder, size, mtime, added) VALUES(?1, ?2, ?3, ?4, ?5)");
    if (ins) upsert(path, &st, ins);
    sqlite3_finalize(ins);
    update_summary();
}
