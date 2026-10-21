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
#include <ctype.h>
#include <strings.h>
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
                               "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value);"
                               "CREATE TABLE IF NOT EXISTS wiki(id INTEGER PRIMARY KEY, page TEXT, about TEXT, credits TEXT,"
                               " reception TEXT, fetched INTEGER)",
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

int albums_sync_list(void (*progress)(const char *what), bool retry_misses, char *err, size_t n) {
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
    exec("DELETE FROM wiki WHERE id NOT IN (SELECT id FROM albums)");
    if (retry_misses) exec("DELETE FROM wiki WHERE page = ''");   /* Sync: Wikipedia asked again about the misses */
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

/* ---- Wikipedia: the album's article ---- */

/* "Art Pepper Meets The Rhythm Section (OJC Remaster)" -> "Art Pepper Meets
   the Rhythm Section": no edition in brackets or after " - ", and the small
   words in lower case, as Wikipedia writes titles (its names are
   case-sensitive past the first letter) */
static void wiki_title(const char *title, char *out, size_t n, bool wiki_case) {
    char t[160];
    snprintf(t, sizeof t, "%s", title);
    for (char *p; (p = strstr(t, " - "));) *p = 0;
    size_t l = strlen(t);
    while (l && (t[l - 1] == ')' || t[l - 1] == ']')) {   /* trailing "(...)" / "[...]" */
        char *o = strrchr(t, t[l - 1] == ')' ? '(' : '[');
        if (!o) break;
        *o = 0;
        l = strlen(t);
        while (l && t[l - 1] == ' ') t[--l] = 0;
    }
    static const char *small[] = { "a", "an", "the", "and", "or", "but", "of", "in", "on", "at", "to", "for", "with", "by", "from", "as", "nor", NULL };
    out[0] = 0;
    int k = 0;
    for (char *w = strtok(t, " "); w; w = strtok(NULL, " "), k++) {
        char word[64];
        snprintf(word, sizeof word, "%s", w);
        if (wiki_case && k)
            for (int i = 0; small[i]; i++)
                if (!strcasecmp(word, small[i])) for (char *c = word; *c; c++) *c = tolower((unsigned char)*c);
        snprintf(out + strlen(out), n - strlen(out), "%s%s", k ? " " : "", word);
    }
}

static net_keep_t s_wiki_conn;

/* a page's summary, if it's an album by this artist ("1957 studio album by
   Art Pepper"); its title into page. busy: no real answer (429, a dropped
   connection, a server error): only "no such page" (404) is one */
static bool album_page(const char *lang, const char *cand, const char *artist, char *page, size_t n, bool *busy) {
    char t[200], enc[400], url[480], err[160] = "";
    snprintf(t, sizeof t, "%s", cand);
    for (char *p = t; *p; p++) if (*p == ' ') *p = '_';
    url_enc(enc, sizeof enc, t, NULL);
    snprintf(url, sizeof url, "https://%s.wikipedia.org/api/rest_v1/page/summary/%s", lang, enc);
    cJSON *j = net_keep_get_json(&s_wiki_conn, url, "Wikipedia", err, sizeof err);
    if (err[0] && !strstr(err, "HTTP 404")) *busy = true;
    const char *type = json_str(j, "type"), *desc = json_str(j, "description"), *text = json_str(j, "extract");
    const char *surname = strrchr(artist, ' ');
    surname = surname ? surname + 1 : artist;
    bool ok = type && !strcmp(type, "standard") && desc && strcasestr(desc, "album") &&
              (strcasestr(desc, surname) || (text && strcasestr(text, surname)));
    if (ok) snprintf(page, n, "%s", json_str(j, "title") ?: cand);
    cJSON_Delete(j);
    return ok;
}

static bool find_page(const char *lang, const album_t *a, char *page, size_t n, bool *busy) {
    char plain[160], wc[160], cand[3][220];
    wiki_title(a->title, plain, sizeof plain, false);
    wiki_title(a->title, wc, sizeof wc, true);
    bool it = !strcmp(lang, "it");
    snprintf(cand[0], sizeof cand[0], "%s", wc);   /* most albums: the plain title */
    snprintf(cand[1], sizeof cand[1], it ? "%s (album %s)" : "%s (%s album)", wc, a->artist);
    snprintf(cand[2], sizeof cand[2], "%s (album)", wc);
    for (int k = 0; k < 3 && !*busy; k++)
        if (album_page(lang, cand[k], a->artist, page, n, busy)) return true;
    if (*busy) return false;
    /* the last resort: one search ("title artist album") */
    char q[300], enc[600], url[760], err[160] = "";
    snprintf(q, sizeof q, "%s %s album", plain, a->artist);
    url_enc(enc, sizeof enc, q, NULL);
    snprintf(url, sizeof url, "https://%s.wikipedia.org/w/api.php?action=query&list=search&srlimit=1&format=json&srsearch=%s", lang, enc);
    cJSON *j = net_keep_get_json(&s_wiki_conn, url, "Wikipedia", err, sizeof err);
    if (err[0]) *busy = true;   /* a search answers, empty or not: an error is no answer */
    char hit[200] = "";
    snprintf(hit, sizeof hit, "%s", json_str(j, "query.search.0.title") ?: "");
    cJSON_Delete(j);
    return hit[0] && album_page(lang, hit, a->artist, page, n, busy);
}

/* the level-2 section whose heading has one of the words (case aside), its
   "=== sub ===" headings made "sub:" lines; at most n - 1 bytes, cut at a
   line with "..." */
static void section(const char *text, const char *const *words, char *out, size_t n) {
    out[0] = 0;
    for (const char *h = strstr(text, "\n== "); h; h = strstr(h + 1, "\n== ")) {
        const char *e = strstr(h + 4, " ==\n");
        if (!e) return;
        bool match = false;
        char head[80];
        snprintf(head, sizeof head, "%.*s", (int)(e - h - 4) < 79 ? (int)(e - h - 4) : 79, h + 4);
        for (int i = 0; words[i] && !match; i++) match = strcasestr(head, words[i]);
        if (!match) continue;
        const char *body = e + 4, *end = strstr(body, "\n== ");
        while (*body == '\n' || *body == ' ') body++;   /* no blank lines in front */
        size_t len = end ? (size_t)(end - body) : strlen(body), k = 0;
        for (size_t i = 0; i < len && k + 4 < n; i++) {
            if (body[i] == '=' ) continue;   /* "=== Ratings ===" -> " Ratings " */
            out[k++] = body[i];
        }
        out[k] = 0;
        if (len >= n - 4) snprintf(out + k, n - k, "...");
        return;
    }
}

/* the article as plain text (one call), cut into intro, credits, reception */
static bool fetch_article(const char *lang, const char *page, album_wiki_t *w) {
    char enc[400], url[560], err[160] = "";
    url_enc(enc, sizeof enc, page, NULL);
    snprintf(url, sizeof url,
             "https://%s.wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=1&exsectionformat=wiki&redirects=1&format=json&titles=%s",
             lang, enc);
    cJSON *j = net_keep_get_json(&s_wiki_conn, url, "Wikipedia", err, sizeof err);
    const cJSON *pages = json_at(j, "query.pages"), *p = pages ? pages->child : NULL;
    const char *text = json_str(p, "extract");
    if (!text) {
        cJSON_Delete(j);
        return false;
    }
    const char *first = strstr(text, "\n== ");
    size_t intro = first ? (size_t)(first - text) : strlen(text);
    snprintf(w->about, sizeof w->about, "%.*s%s", (int)(intro < sizeof w->about - 4 ? intro : sizeof w->about - 4), text,
             intro >= sizeof w->about - 4 ? "..." : "");
    static const char *const personnel[] = { "personnel", "credits", "musicians", "formazione", "musicisti", NULL };
    static const char *const reception[] = { "reception", "critic", "review", "accoglienza", "critica", NULL };
    section(text, personnel, w->credits, sizeof w->credits);
    section(text, reception, w->reception, sizeof w->reception);
    cJSON_Delete(j);
    return true;
}

static bool wiki_cached(long long id, album_wiki_t *w) {
    bool found = false;
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    /* "nothing" counts for 30 days: an article may come, or a better match */
    if (open_db() && sqlite3_prepare_v2(s_db, "SELECT page, about, credits, reception FROM wiki WHERE id = ?1"
                                              " AND (page != '' OR fetched > ?2)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_int64(st, 2, (long long)time(NULL) - 30 * 86400);
        if ((found = sqlite3_step(st) == SQLITE_ROW)) {
            snprintf(w->page, sizeof w->page, "%s", (const char *)sqlite3_column_text(st, 0) ?: "");
            snprintf(w->about, sizeof w->about, "%s", (const char *)sqlite3_column_text(st, 1) ?: "");
            snprintf(w->credits, sizeof w->credits, "%s", (const char *)sqlite3_column_text(st, 2) ?: "");
            snprintf(w->reception, sizeof w->reception, "%s", (const char *)sqlite3_column_text(st, 3) ?: "");
        }
    }
    sqlite3_finalize(st);
    index_sqlite_unlock();
    return found;
}

static void wiki_store(long long id, const album_wiki_t *w) {
    index_sqlite_lock();
    sqlite3_stmt *st = NULL;
    if (open_db() && sqlite3_prepare_v2(s_db, "INSERT OR REPLACE INTO wiki VALUES(?1, ?2, ?3, ?4, ?5, ?6)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_text(st, 2, w->page, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, w->about, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, w->credits, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, w->reception, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 6, time(NULL));
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    index_sqlite_unlock();
}

bool albums_wiki(const album_t *a, album_wiki_t *w) {
    memset(w, 0, sizeof *w);
    if (wiki_cached(a->id, w)) return true;
    const char *langs[] = { "en", CONFIG_ARTSHOW_WIKI_LANG };
    bool busy = false, found = false;
    for (int k = 0; k < 2 && !found && !busy; k++) {
        if (k && !strcmp(langs[1], "en")) break;
        if (find_page(langs[k], a, w->page, sizeof w->page, &busy) && !(found = fetch_article(langs[k], w->page, w)))
            busy = true;   /* the page is there, its text didn't come: another time */
    }
    net_keep_close(&s_wiki_conn);
    if (busy) return false;   /* asked again another time */
    if (!found) memset(w, 0, sizeof *w);
    wiki_store(a->id, w);   /* found or not: once */
    ESP_LOGI(TAG, "%s: Wikipedia %s", a->title, found ? w->page : "has nothing");
    return true;
}
