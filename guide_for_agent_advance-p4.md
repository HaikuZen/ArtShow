# Building an app for the Elecrow CrowPanel Advanced 7" / 10.1" (ESP32-P4): a guide for agents

What an agent needs to know before writing code for this board, distilled
from building ArtShow (a slideshow, eleven pages, Wi-Fi, HTTPS APIs, SQLite,
audio) on a bench board. Each rule here was paid for: most come with the
symptom that taught it. ESP-IDF 6.1, LVGL 9.6, esp_hosted 2.12.x.

The board's wizard and its cookbook (every value explained):
https://github.com/HaikuZen/advance-p4-wiz, `docs/advance-p4-cookbook.md`.

---

## 1. The board in one table

| part | what it is | what to know |
|---|---|---|
| SoC | ESP32-P4, **rev v1.x** (bench: v1.3), 2 x RISC-V 360 MHz | not rev 3: see 2.1 |
| memory | 16 MB flash, 32 MB hex PSRAM at 200 MHz, ~500 KB internal RAM | internal RAM is the scarce one: see 5 |
| display | 1024 x 600 MIPI-DSI, EK79007, 2 lanes x 900 Mbps, 51 MHz DPI | landscape only: the panel can't swap x/y |
| touch | GT911 on I2C (SDA 45, SCL 46, RST 40, INT 42), address 0x5d | |
| backlight | MT9201 driver, PWM on GPIO 31 | flickers below ~25 %: see 4.3 |
| Wi-Fi / BLE | an onboard **ESP32-C6** over 4-bit SDIO (slot 1), esp_hosted | the P4 has no radio: see 6 |
| microSD | SDMMC **slot 0**, 1-bit, CLK 43, CMD 44, D0 39, IO from LDO4 | shares the host with the C6: see 7 |
| audio | I2S straight into two speaker amps: BCLK 22, WS 21, DOUT 23; amps' enable GPIO 30, **active low** | **no codec** (the schematic's ES8311 isn't fitted) |
| I2C bus | GT911 (0x5d) and an STC8 helper (0x2f), nothing else | |
| USB | J16 = the P4's high-speed USB OTG (DP/DM), **VBUS is an input only** (diodes) | no 5 V out: a device must be self-powered or on a powered hub |
| console | UART0 on GPIO 37/38 = the CH340, the "UART" USB socket | flash and monitor through it |
| BOOT key | GPIO 35 | GPIO 0/1 are the 32 kHz crystal |
| D-PHY power | the P4's internal LDO3 at 2.5 V | no LDO3, no picture |

Keep every pin in one header (`board_pins.h`) and never change a value
without a reason from the hardware.

---

## 2. Settings that make or break the build

### 2.1 Chip revision: the image must be built for rev < 3

IDF 6.x builds for rev >= 3.1 by default. Such an image **builds fine and
refuses to boot** on this board. In `sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y
```

Check a board with `esptool.py -p <port> -b 460800 chip_id`.

The DSI clock follows the revision: use
`MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY`. IDF 6.1's plain `_DEFAULT` is
XTAL, which only rev >= 3 has: `esp_lcd_new_dsi_bus` aborts at boot.

### 2.2 The rest of `sdkconfig.defaults`

```
# memory
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y          # TLS buffers (~40 KB a connection) in PSRAM
# CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY is not set   # rev v1.x has no ECDSA peripheral

# console on the CH340
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y

# LVGL 9
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_USE_CUSTOM_MALLOC=y                # the system heap, PSRAM first: see 5
CONFIG_LV_USE_PPA=y                          # the P4's 2D engine draws fills and images
CONFIG_LV_USE_PPA_IMG=y
CONFIG_LV_DRAW_BUF_ALIGN=64                  # the PPA wants cache-line (64 B) alignment
CONFIG_LV_FONT_MONTSERRAT_<n>=y              # one line per size used

# Wi-Fi through the C6 (esp_hosted 2.12.x names!)
CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED=y
CONFIG_SLAVE_IDF_TARGET_ESP32C6=y
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CLK_SLOT_1=18
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_CMD_SLOT_1=19
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D0_SLOT_1=17
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D1_4BIT_BUS_SLOT_1=16
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D2_4BIT_BUS_SLOT_1=15
CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_D3_4BIT_BUS_SLOT_1=14
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=32
CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y
CONFIG_ESP_HOSTED_DFLT_TASK_FROM_SPIRAM=y

# FAT on the card: long, accented names
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_API_ENCODING_UTF_8=y

CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

A partition table with room for a big app (LVGL, fonts, TLS, SQLite reach
~3 MB quickly):

```
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x400000,
```

### 2.3 Silent failures in the config

- **A changed `sdkconfig.defaults` does nothing** until `sdkconfig` is
  deleted and the project rebuilt. Then check that every line made it in
  (`p4wiz.py --check .` does it).
- **Grep every build log for `unknown kconfig symbol`**: it means a line
  was dropped without an error. The classic case is esp_hosted 3.x, which
  renamed every option: the 2.x names above still build under 3.x, but
  with the wrong SDIO pins and C6 reset pin. Then check that `sdkconfig`
  has D0 = 17 and reset = 32.
- Keep the build free of warnings: a warning is often the only sign of a
  deprecated API (LVGL 9.6 deprecates a lot, see 4.1) or a wrong type.

### 2.4 Components (`main/idf_component.yml`)

```yaml
dependencies:
  idf: ">=6.0"
  espressif/esp_lcd_ek79007: "^2.0.2"
  espressif/esp_lcd_touch_gt911: "^1.2.1"
  espressif/esp_lvgl_port: "^2.9.0"
  lvgl/lvgl: "^9.6.0"
  espressif/esp_hosted:
    version: "~2.12.3"               # matches the C6's firmware (Elecrow ships 2.12.3)
    rules: [ { if: "target == esp32p4" } ]
  espressif/esp_wifi_remote:
    version: "1.6.3"                 # the last one that keeps esp_hosted < 3.0
    rules: [ { if: "target == esp32p4" } ]
  espressif/esp_new_jpeg: "^1.0.2"   # software JPEG, decodes and encodes, can shrink
  espressif/cjson: "^1.7.19"         # IDF 6 has no json component
```

The manifest **must be in `main/`**: one at the project root is silently
ignored.

### 2.5 Paths

No `[` or `]` anywhere in the project's path: LVGL's source glob breaks.

---

## 3. Build, flash, test

```sh
. ~/.espressif/tools/activate_idf_v6.1.sh      # or export.sh
idf.py set-target esp32p4 build                # once; then build alone
idf.py -p /dev/ttyUSB<n> -b 460800 flash monitor
```

- Flash through the **UART** socket (CH340) at **460800**: 921600
  corrupted reads on this board. If it won't enter the bootloader: hold
  BOOT, press RESET, let go of BOOT.
- **Check the build's exit code before flashing.** A failed build followed
  by `flash` writes the *previous* binary, and the test then "passes" or
  "fails" on old code.
- **Flashing replaces what the board runs**: ask first, and offer a backup:
  `esptool.py -p <port> -b 460800 read_flash 0 0x1000000 backup.bin`. That
  backup (and every `build/*.bin`) holds the Wi-Fi password in plain text:
  never share or upload them.
- Capture logs without an interactive monitor with a small pyserial script
  that pulses RTS (reset) and reads for N seconds into a file, then grep it.
- Test a page on the board by opening it from **an `lv_timer` in the LVGL
  task** (one-shot, a few seconds after boot), not by calling the menu from
  `app_main`: the main task's stack is small, and a page that opens a
  database there overflowed it ("Stack protection fault").
- Mark temporary test code (`/* TEST */`), keep a copy of the file before
  adding it, and restore and rebuild before committing. Log
  `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`, its largest block and its
  minimum ever, and PSRAM's, every few seconds while testing anything new.
- Tests that write to the card leave files behind (a test that pressed
  Save put its pictures in the slideshow): delete them after.
- Pure computation (image effects, painters, synthesis, astronomy) goes in
  plain C files with no IDF includes, so it builds and runs on the computer
  first: render a 1024 x 600 RGB565 file or a WAV there, look at it, then
  put it on the board.
- Free API plans count calls, and every test boot fetches: a day of test
  reboots used up a 50-calls-a-day weather key.

### Harmless log lines

- `W gpio: conflict found for GPIO[42]`: the GT911's INT pin. Touch works.
- `E lcd_panel: swap_xy is not supported by this panel`: esp_lvgl_port asks
  the EK79007. Nothing breaks.
- `W os_wrapper_esp: CONFIG_FREERTOS_HZ is 100, ESP-Hosted recommended 1000`:
  Wi-Fi works at 100.

---

## 4. Display, LVGL and touch

### 4.1 LVGL 9.6 rules

- **Every LVGL call from outside the LVGL task holds `lvgl_port_lock(0)`**
  (it waits forever; the lock is recursive). Callbacks from other tasks
  (Wi-Fi, workers) take it before touching any object.
- Use 9.6's API, or the build warns: the per-flag setters
  (`lv_obj_set_hidden`, `lv_obj_set_clickable`, `lv_obj_set_overflow_visible`,
  `lv_obj_is_hidden` ...), not `lv_obj_add_flag`/`has_flag`;
  `LV_LABEL_LONG_MODE_*`; `lv_list` is deprecated (build lists as a flex
  column); `lv_draw_task_get_draw_dsc()` (the task's fields are private).
- Built-in Montserrat fonts are **ASCII only**. For accented names, typographic
  quotes and dashes, convert fonts with Latin-1 and Latin Extended-A
  (`lv_font_conv`). Montserrat 16's line height is 22 px: size labels by
  the font's `line_height`, not its nominal size.
- `LV_LABEL_LONG_MODE_DOTS` only shortens a label **with a fixed height**;
  without one it wraps over the line below.
- `lv_calendar`'s highlight is a fill on the day's cell, and no fill is
  drawn on a transparent cell: make the cells opaque.
- LVGL keeps 32 invalid areas a frame: invalidate in bands, not per tile.
- Custom drawing: `LV_EVENT_DRAW_MAIN` (behind the children),
  `DRAW_MAIN_END`/`DRAW_POST` (over), `LV_EVENT_DRAW_TASK_ADDED` to restyle
  what a widget draws (needs the send-draw-task-events flag). Drawing
  shapes this way (frames, icons, clock hands, a record) costs no memory
  and scales; a full-screen texture is better as one small tile used as a
  tiled background image (`lv_obj_set_style_bg_image_tiled`).
- Thousands of objects don't fit internal RAM: make a page's cells once and
  page through the data.

### 4.2 Speed and the PPA

- Images the PPA draws (`CONFIG_LV_USE_PPA_IMG`) must be RGB565 buffers
  **64-byte aligned** (`heap_caps_aligned_alloc(64, ..., MALLOC_CAP_SPIRAM)`),
  with rows whose stride is a multiple of 64 bytes. A full-screen image by
  the CPU took ~40 ms; by the PPA, a fraction.
- PSRAM bandwidth is the limit: the panel's scan-out, LVGL's framebuffers
  and every big buffer share it. A full-screen 1.2 MB PSRAM copy is ~28 ms;
  a full-screen redraw per frame gives 7-12 fps. Redraw only what changed.
- LVGL from the system heap: drawing a busy page takes ~125 KB for a
  moment. Give LVGL a PSRAM-first allocator (see 5), or leave room for it.
- With LVGL's direct mode and two framebuffers, the off-screen buffer lags
  until the next refresh: comparing it with the picture shows differences,
  and that is normal.

### 4.3 Backlight

- PWM on GPIO 31 into an MT9201. Below ~25 % the screen flickers (worse on
  white), at any PWM frequency from 1 to 30 kHz: keep 25 % as the floor of
  any brightness control, and use 0 (off) for "dark".

### 4.4 Designing pages for this screen

- 1024 x 600, touch: hit targets 44 px or more; extend small ones with
  `lv_obj_set_ext_click_area`.
- Corner buttons that live on every page (menu top left, brightness bottom
  left, Wi-Fi bottom right in ArtShow) are styled for dark grounds: on a
  light page they vanish. Keep a dark margin under them, or keep content
  out of the corners (y > ~530 at the sides is theirs).
- Long text scrolls in a fixed box; fixed-height one-line labels for
  captions.

---

## 5. Memory: internal RAM is the scarce resource

The P4 has ~500 KB of internal RAM and 32 MB of PSRAM. What ran out, every
time, was **internal RAM**, while PSRAM had 15 MB free.

What uses internal RAM:
- **task stacks** (by default), LVGL objects and its draw buffers in
  flight, lwIP and esp_hosted buffers, FreeRTOS objects;
- each open file on the card: a ~4 KB cache (`CONFIG_FATFS_PER_FILE_CACHE`);
- DMA bounce buffers: the SD driver allocates one (internal) for a read
  into memory it can't DMA into;
- TLS: even with `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`, the handshakes
  themselves need internal RAM.

The symptom of running out is **not** a clean error where you allocate:
- TLS fails with `-0x008D` (PSA_ERROR_INSUFFICIENT_MEMORY) in
  `mbedtls_ssl_setup` or the handshake;
- `sdmmc_cmd: allocate_dma_buf: not enough mem` and a failed card read;
- LVGL pages that half-draw.

Rules:
- **Big buffers in PSRAM**, explicitly: `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
  (images, canvases, text, JSON, tables, caches). cJSON via
  `cJSON_InitHooks` into PSRAM: a 70 KB JSON answer is ~3000 nodes, ~200 KB
  of internal RAM otherwise. SQLite's memory too (`SQLITE_CONFIG_MALLOC`).
- **Worker task stacks in PSRAM**: `xTaskCreateWithCaps(fn, name, stack,
  arg, prio, &handle, MALLOC_CAP_SPIRAM)` (needs
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`). Allowed only for tasks that
  **never write flash** (NVS, OTA, partitions): a flash write disables the
  cache, and a PSRAM stack with it. Moving four network workers' stacks
  (42 KB) took the lowest free internal RAM from 4 KB to ~40 KB.
- Keep few files open (`max_files` small), open databases only while used.
- Static arrays in the app (`static uint16_t big[...]`) land in internal
  RAM: allocate them in PSRAM at start instead.
- **LVGL's memory in PSRAM**: with `CONFIG_LV_USE_CLIB_MALLOC`, every object,
  local style and draw buffer under 16 KB (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`)
  lands in internal RAM, and a dozen styled pages took ~35 KB: at boot free
  internal RAM fell to 10 KB, TLS failed and an SD read lost its DMA buffer.
  Set `CONFIG_LV_USE_CUSTOM_MALLOC=y` and supply `lv_malloc_core`,
  `lv_realloc_core`, `lv_free_core` (plus `lv_mem_init`, `lv_mem_deinit`,
  `lv_mem_add_pool`, `lv_mem_remove_pool`, `lv_mem_monitor_core`,
  `lv_mem_test_core`) with `heap_caps_malloc_prefer(size, 2,
  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.
  Measured: the lowest free internal RAM went from 10 KB to 84 KB, page
  draws and transitions the same within their spread. Only LVGL calls these,
  and libmain links before LVGL: force the file in with
  `target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,lv_malloc_core")`.
- **No shadows on overlays over animated content**: a shadow allocates its
  buffers each time it's redrawn, and an open menu over a slideshow
  transition is redrawn every frame (an SD read lost its DMA buffer once).
  A dark outline costs nothing.
- **Measure**: log free, largest block and minimum-ever internal RAM while
  Wi-Fi connects, TLS runs and pages draw at the same time. Aim for 40 KB+
  at the lowest; ~90 KB free when idle is comfortable.

---

## 6. Wi-Fi through the ESP32-C6

- The P4 has no radio. esp_hosted talks to the C6 over SDIO, and
  esp_wifi_remote forwards the normal `esp_wifi_*` API to it: **every
  `esp_wifi_*` call is an RPC to the C6**. esp_hosted's link starts inside
  `esp_wifi_init`.
- One Wi-Fi task owns every `esp_wifi_*` call; others post it messages and
  get callbacks. Don't call `esp_wifi_*` from the UI or several tasks.
- **No `ESP_ERROR_CHECK` in Wi-Fi code**: a failed step logs and turns
  Wi-Fi off; it never reboots the app. Rejoin after a drop with a backoff
  (1, 2, 4 ... 30 s).
- Disconnect reasons: 15 handshake timeout (often a wrong password), 201
  no AP found, 202 auth failed, 8 we left.
- **Credentials**: a network entered on screen goes to NVS; a fallback can
  be set in `menuconfig` (it lands in `sdkconfig`, gitignored). **Never put
  an SSID or password in `sdkconfig.defaults` or a source file**: they're
  committed. The password is in plain text in `build/*.bin` and in flash.
- SNTP: start it after `wifi_start` (which inits esp_netif), and **restart
  it on each `IP_EVENT_STA_GOT_IP`**: its first tries fail before there's
  an IP, and it then waits ~27 s to retry; restarted, the time comes ~3 s
  after the IP. Set `TZ` first thing in `app_main` if anything stamps local
  times before the sync.

### TLS on rev v1.x

- HTTPS with `esp_http_client` and IDF's certificate bundle.
- The rev v1.x P4 has no ECDSA peripheral: a handshake verifies in software,
  **~2 s each**, longer when several overlap. Keep a connection open for a
  run of calls to one server; cache answers.
- A handshake is seconds of CPU without a yield: **run network tasks at
  idle priority** (`tskIDLE_PRIORITY`). At priority 3 one starved core 1's
  idle task and the task watchdog fired, over and over.
- Never log a URL that carries an API key; keep keys on the SD card
  (`config.json`), never in sources.
- Public APIs rate-limit (Wikipedia's search: HTTP 429 for minutes after a
  burst). One search per lookup, cache answers, show "busy, try again".
- `esp_http_client` didn't follow a relative 302 on the board: don't rely
  on it for odd redirects.

---

## 7. The microSD card (and why it fights Wi-Fi)

- SDMMC **slot 0**, 1-bit (CLK 43, CMD 44, D0 39; D1..D3 can't be used:
  40..42 are touch and LCD lines), internal pull-ups, IO powered by LDO4
  at 3.3 V (take it in board init). 20 MHz is fine; 10 MHz if a card is flaky.
- **Set `host.slot = SDMMC_HOST_SLOT_0` by hand**: on the P4
  `SDMMC_HOST_DEFAULT()` picks slot 1. A mount on slot 1 fails, its cleanup
  removes esp_hosted's slot, esp_hosted asserts, and the board reboots in
  a loop.
- esp_hosted owns **slot 1 of the same SDMMC host**: never deinit the host,
  only slot 0.
- **Mount the card once, before `wifi_start()`.** A card init changes the
  controller's clock; doing it while the C6 link is up (or coming up) times
  out (0x107), breaks the link ("Unrecoverable host sdio state") and
  restarts the board. Reads and writes once both are up are fine. No
  remounts: if the card goes, say so and offer a restart.
- **A card left mid-read** by a board reset (the card keeps its power)
  holds D0 low, and the first clock update jams the whole host (Wi-Fi
  too). Before mounting, clock the card by hand and send CMD0 (an
  "unstick" step).
- FAT and a board that is reset, never shut down: a file's size reaches the
  card only when it's synced or closed. SQLite on FAT needs
  `PRAGMA synchronous = FULL` (with NORMAL the next boot read the database
  as "disk I/O error"). FAT has no locks: one connection, one thread
  (`SQLITE_THREADSAFE=0` plus a mutex), no WAL, temp in memory. A database
  that's only a cache may be rebuilt when unreadable; one with the user's
  data must be renamed aside and kept, never replaced.
- 1-bit SD is slow: ~1.5-3 s to read a 3 MB file (much longer when the CPU is
  busy). Stream downloads to the card in chunks (16 KB).

---

## 8. Tasks, cores and the watchdog

- FreeRTOS ticks at 100 Hz here: `vTaskDelay(1)` is 10 ms. A loop that
  works in 25 ms slices and yields a tick loses a third of its time: use
  ~100 ms slices.
- **Anything that computes for seconds without blocking** (a software JPEG
  decode: 3-9 s, up to 60 s under load; image effects; procedural painting;
  TLS handshakes) runs at **`tskIDLE_PRIORITY`**, or yields often. At any
  higher priority it starves that core's idle task and the task watchdog
  fires (`task_wdt: ... IDLE1 (CPU 1)`). Equal-priority tasks share the
  core round-robin, so idle priority still gets its work done.
- Pin CPU-heavy work to core 1 (LVGL, Wi-Fi and esp_hosted are busy on
  core 0); audio rendering to core 0 if a painter is on core 1.
- Work that the UI waits for (thumbnails, page data) goes in a worker task
  with a request queue; a newer request makes older ones stale (a sequence
  number), and results are applied under the LVGL lock.

---

## 9. Images and JPEG

- **Hardware JPEG engine**: fast (~50 ms), but **baseline only**, can't
  scale, and the full-size output must fit in memory (8 MB of RGB565 is ~4 MP).
- **esp_new_jpeg** (software): shrinks 1/2, 1/4, 1/8 while decoding, so a
  4096 px picture fits; ~3-9 s. Also baseline only (progressive JPEGs:
  skip them, and say why). It also encodes (quality 75-92; 4:4:4 keeps thin
  coloured lines). EXIF rotation is not applied.
- Box-scale into the target size afterwards; keep output buffers 64-byte
  aligned for the PPA.

## 10. Maths and speed on the P4 (measured)

- The FPU is **single precision**: doubles are software (fine for a few
  thousand operations, e.g. astronomy once a day; not per pixel).
- A float op ~6 cycles; `sinf` ~6 us (a 4096-entry table is 25x faster);
  `logf` ~0.35 us (use a table for per-pixel tone maps); random writes to a
  PSRAM buffer ~0.9 us each.
- Build pixel loops at `-O2` (`set_source_files_properties(... COMPILE_OPTIONS
  "-O2")`) even when the app is `-Og` (a diffusion filter went from 94 s to
  15 s with -O2 and a tighter loop together). Interleave channels,
  use fixed point where it's simpler, flush denormals.
- Paint big pictures off the LVGL task, in place, and invalidate when done.

## 11. Audio

- No codec: plain I2S (standard mode, 16 bit) straight into the amps. Turn
  the amps on (GPIO 30 **low**) only while playing: off they're silent.
- Enable the channel's auto-clear so gaps between writes are silence, not
  a buzz. Feed the DMA from one task in short blocks (e.g. 16 ms); the DMA
  paces it.
- USB audio out (a USB DAC on J16): the P4's host can do UAC 1.0, but the
  socket gives no 5 V (self-powered DAC or powered hub), split transactions
  aren't supported (a full-speed DAC behind a high-speed hub fails), and
  IDF 6.1 has no full-speed-only option.

## 12. Housekeeping for agents

- Ask before flashing; offer a backup; never upload builds or backups.
- Commit only when asked. Never commit `sdkconfig`, `build/`, keys, IDs
  that belong to the user (put them in `config.json` on the card).
- After each change: build (exit code!), no warnings, no
  `unknown kconfig symbol`, flash, capture the log, grep it for `Guru`,
  `abort`, `assert`, `task_wdt`, `not enough mem`, `-0x`, and your own
  module's lines. Report what was measured, and say what wasn't seen
  (an agent can't see the screen: ask the user to look).
- Write down what each pitfall cost (symptom, cause, fix) where the next
  agent will read it (the project's `CLAUDE.md`).
