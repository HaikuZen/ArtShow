/* notes.c - notes(day TEXT PRIMARY KEY "2026-10-20", text, updated). Opened at
 * the first call, under index.c's SQLite lock. Synced at each commit
 * (synchronous FULL), as the index: on FAT a file's size reaches the card
 * only when synced, and the board is reset, never closed. */
#include "notes.h"
#include "index.h"
#include "sqlite3.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DIR_       "/sdcard/.artshow"
#define NOTES_PATH DIR_ "/notes.db"

static const char *TAG = "notes";

static sqlite3 *s_db;

static bool open_db(void) {
    if (sqlite3_open(NOTES_PATH, &s_db) == SQLITE_OK &&
        sqlite3_exec(s_db, "PRAGMA journal_mode = TRUNCATE; PRAGMA synchronous = FULL;"
                           "CREATE TABLE IF NOT EXISTS notes(day TEXT PRIMARY KEY, text TEXT NOT NULL, updated INTEGER)",
                     NULL, NULL, NULL) == SQLITE_OK)
        return true;
    sqlite3_close(s_db);
    s_db = NULL;
    return false;
}

/* open it the first time; one that can't be read is renamed, kept for a
   look on a computer, and a new one made */
static bool ready(void) {
    if (s_db) return true;
    if (mkdir(DIR_, 0777) && errno != EEXIST) return false;
    if (open_db()) return true;
    char aside[80];
    snprintf(aside, sizeof aside, DIR_ "/notes-unreadable-%lld.db", (long long)time(NULL));
    ESP_LOGW(TAG, "%s can't be read: kept as %s, and a new one", NOTES_PATH, aside);
    rename(NOTES_PATH, aside);
    remove(NOTES_PATH "-journal");
    return open_db();
}

/* closed after each call: notes are seldom read or written, and an open
   SQLite file and its journal hold a 4 KB cache each in internal RAM */
static void done(void) {
    sqlite3_close(s_db);
    s_db = NULL;
}

static void key(char *buf, size_t n, int y, int m, int d) { snprintf(buf, n, "%04d-%02d-%02d", y, m, d); }

bool notes_get(int year, int month, int day, char *buf, size_t n) {
    buf[0] = 0;
    char k[16];
    key(k, sizeof k, year, month, day);
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (ready() && sqlite3_prepare_v2(s_db, "SELECT text FROM notes WHERE day = ?1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) snprintf(buf, n, "%s", (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    done();
    index_sqlite_unlock();
    return buf[0];
}

bool notes_set(int year, int month, int day, const char *text) {
    char k[16];
    key(k, sizeof k, year, month, day);
    while (*text == ' ' || *text == '\n') text++;   /* only blanks: no note */
    index_sqlite_lock();
    bool ok = false;
    sqlite3_stmt *st = NULL;
    const char *sql = *text ? "INSERT OR REPLACE INTO notes VALUES(?1, ?2, ?3)" : "DELETE FROM notes WHERE day = ?1";
    if (ready() && sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
        if (*text) {
            sqlite3_bind_text(st, 2, text, -1, SQLITE_STATIC);
            sqlite3_bind_int64(st, 3, time(NULL));
        }
        ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok) ESP_LOGW(TAG, "%s: %s", k, sqlite3_errmsg(s_db));
    }
    sqlite3_finalize(st);
    done();
    index_sqlite_unlock();
    if (ok) ESP_LOGI(TAG, "%s: %s", k, *text ? "saved" : "deleted");
    return ok;
}

int notes_days(notes_date_t *out, int max) {
    int n = 0;
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (ready() && sqlite3_prepare_v2(s_db, "SELECT day FROM notes ORDER BY day", -1, &st, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(st) == SQLITE_ROW) {
            int y, m, d;
            if (sscanf((const char *)sqlite3_column_text(st, 0), "%d-%d-%d", &y, &m, &d) == 3)
                out[n++] = (notes_date_t){ (short)y, (signed char)m, (signed char)d };
        }
    }
    sqlite3_finalize(st);
    done();
    index_sqlite_unlock();
    return n;
}
