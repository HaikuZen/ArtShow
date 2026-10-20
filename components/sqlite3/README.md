# sqlite3

SQLite 3.54.1: `sqlite3.c` and `sqlite3.h` from
https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip, unchanged
(SHA3-256 of the zip, as sqlite.org lists it:
92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30).
Public domain.

`sqlite3_esp.c`: the VFS (after SQLite's `test_demovfs.c`): POSIX
open/read/write/lseek/fsync/ftruncate through ESP-IDF's VFS, so any mounted
filesystem (FAT on the microSD card here). No file locking: one task opens
the database. `CMakeLists.txt` has the compile options.

To update: replace the two files with a newer amalgamation's and check the
build (and that `SQLITE_OS_OTHER` still needs only `sqlite3_os_init/end`).
