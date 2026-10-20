/* sqlite3_esp.c - a VFS for SQLite on ESP-IDF, after SQLite's test_demovfs.c:
 * plain POSIX file calls, which ESP-IDF's VFS routes to the filesystem
 * (FAT, on the card). No locking (one task uses the database) and no
 * shared memory (no WAL). */
#include "sqlite3.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    sqlite3_file base;
    int fd;
} esp_file_t;

static int f_close(sqlite3_file *f) {
    close(((esp_file_t *)f)->fd);
    return SQLITE_OK;
}

static int f_read(sqlite3_file *f, void *buf, int n, sqlite3_int64 off) {
    int fd = ((esp_file_t *)f)->fd;
    if (lseek(fd, off, SEEK_SET) != off) return SQLITE_IOERR_READ;
    int got = 0;
    while (got < n) {
        int r = read(fd, (char *)buf + got, n - got);
        if (r < 0) return SQLITE_IOERR_READ;
        if (r == 0) break;
        got += r;
    }
    if (got == n) return SQLITE_OK;
    memset((char *)buf + got, 0, n - got);   /* SQLite wants the rest zeroed */
    return SQLITE_IOERR_SHORT_READ;
}

static int f_write(sqlite3_file *f, const void *buf, int n, sqlite3_int64 off) {
    int fd = ((esp_file_t *)f)->fd;
    if (lseek(fd, off, SEEK_SET) != off) return SQLITE_IOERR_WRITE;
    for (int put = 0; put < n;) {
        int w = write(fd, (const char *)buf + put, n - put);
        if (w <= 0) return SQLITE_IOERR_WRITE;
        put += w;
    }
    return SQLITE_OK;
}

static int f_truncate(sqlite3_file *f, sqlite3_int64 size) {
    return ftruncate(((esp_file_t *)f)->fd, size) ? SQLITE_IOERR_TRUNCATE : SQLITE_OK;
}

static int f_sync(sqlite3_file *f, int flags) {
    return fsync(((esp_file_t *)f)->fd) ? SQLITE_IOERR_FSYNC : SQLITE_OK;
}

static int f_size(sqlite3_file *f, sqlite3_int64 *size) {
    struct stat st;
    if (fstat(((esp_file_t *)f)->fd, &st)) return SQLITE_IOERR_FSTAT;
    *size = st.st_size;
    return SQLITE_OK;
}

static int f_lock(sqlite3_file *f, int level) { return SQLITE_OK; }
static int f_unlock(sqlite3_file *f, int level) { return SQLITE_OK; }
static int f_reserved(sqlite3_file *f, int *out) {
    *out = 0;
    return SQLITE_OK;
}
static int f_control(sqlite3_file *f, int op, void *arg) { return SQLITE_NOTFOUND; }
static int f_sector(sqlite3_file *f) { return 512; }
static int f_caps(sqlite3_file *f) { return 0; }

static const sqlite3_io_methods s_io = {
    1, f_close, f_read, f_write, f_truncate, f_sync, f_size,
    f_lock, f_unlock, f_reserved, f_control, f_sector, f_caps,
};

static int v_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *out_flags) {
    if (!name) return SQLITE_IOERR;   /* temporary files: SQLITE_TEMP_STORE=3 keeps them in memory */
    int o = flags & SQLITE_OPEN_READWRITE ? O_RDWR : O_RDONLY;
    if (flags & SQLITE_OPEN_CREATE) o |= O_CREAT;
    if (flags & SQLITE_OPEN_EXCLUSIVE) o |= O_EXCL;
    int fd = open(name, o, 0666);
    if (fd < 0) return SQLITE_CANTOPEN;
    esp_file_t *f = (esp_file_t *)file;
    memset(f, 0, sizeof *f);
    f->fd = fd;
    f->base.pMethods = &s_io;
    if (out_flags) *out_flags = flags;
    return SQLITE_OK;
}

static int v_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
    return unlink(name) && errno != ENOENT ? SQLITE_IOERR_DELETE : SQLITE_OK;
}

static int v_access(sqlite3_vfs *vfs, const char *name, int flags, int *out) {
    struct stat st;
    *out = stat(name, &st) == 0;
    return SQLITE_OK;
}

static int v_fullpath(sqlite3_vfs *vfs, const char *name, int n, char *out) {
    sqlite3_snprintf(n, out, "%s", name);   /* the paths are absolute already: "/sdcard/..." */
    return SQLITE_OK;
}

static int v_random(sqlite3_vfs *vfs, int n, char *out) {
    esp_fill_random(out, n);
    return n;
}

static int v_sleep(sqlite3_vfs *vfs, int us) {
    vTaskDelay(pdMS_TO_TICKS(us / 1000) + 1);
    return us;
}

static int v_time(sqlite3_vfs *vfs, double *jd) {
    *jd = time(NULL) / 86400.0 + 2440587.5;
    return SQLITE_OK;
}

static int v_error(sqlite3_vfs *vfs, int n, char *out) { return 0; }

static sqlite3_vfs s_vfs = {
    1, sizeof(esp_file_t), 256, NULL, "esp", NULL,
    v_open, v_delete, v_access, v_fullpath,
    NULL, NULL, NULL, NULL,   /* no extensions to load */
    v_random, v_sleep, v_time, v_error,
};

/* SQLITE_OS_OTHER: SQLite calls these to set up (and tear down) its OS layer */
int sqlite3_os_init(void) { return sqlite3_vfs_register(&s_vfs, 1); }
int sqlite3_os_end(void) { return SQLITE_OK; }
