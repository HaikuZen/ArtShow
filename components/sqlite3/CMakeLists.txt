# SQLite 3.54.1, the amalgamation as published (README.md), with a VFS for
# ESP-IDF's file API (sqlite3_esp.c): SQLITE_OS_OTHER, no locking.
idf_component_register(SRCS "sqlite3.c" "sqlite3_esp.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_hw_support freertos)
set(SQLITE_DEFS
    SQLITE_OS_OTHER=1               # our VFS, not unix's (no fcntl locks on FAT)
    SQLITE_THREADSAFE=0             # one task uses it: the slideshow's
    SQLITE_TEMP_STORE=3             # temporary tables in memory, never on the card
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_CACHE_SIZE=-256  # 256 KB of page cache
    SQLITE_DEFAULT_LOCKING_MODE=1   # exclusive: nobody else opens it
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_WAL                 # needs shared memory
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_PROGRESS_CALLBACK
    SQLITE_OMIT_JSON
    SQLITE_MAX_EXPR_DEPTH=100       # the parser's recursion: a small task stack
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${SQLITE_DEFS})
# third-party code: its warnings aren't ours to fix (as for LVGL)
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
