# ArtShow

An ESP-IDF 6.x app for the **Elecrow CrowPanel Advanced 7" / 10.1"** (ESP32-P4,
hardware V1.2), scaffolded by p4wiz on 2026-10-18 with: Wi-Fi (esp_hosted 2.12.x), audio.

The wizard and its full recipe live in the advance-p4-wiz repo
(https://github.com/HaikuZen/advance-p4-wiz; here: `/root/Scaricati/CYD/advance-p4-wiz`). Its
`docs/advance-p4-cookbook.md` explains every board value and pitfall below.

## Files

| path | what |
|---|---|
| `main/main.c` | `app_main`: `board_init()`, the card mount (before Wi-Fi) and `config.json`, NVS, then (under the LVGL lock) the slideshow, Now Showing (its "?"), Diffuse (its drop), the gallery, the clock, weather, sun and moon, artwork, generative, album wall and settings pages, the Wi-Fi button, the brightness slider and the menu; then Wi-Fi, NTP and the weather and artwork tasks |
| `main/menu.c/.h` | the hamburger menu, top left: a table of pages (`menu_page_t` in `main.c`), one shown at a time, up to `MAX_PAGES` (16; more are dropped). New page: a `create` that returns a hidden full-screen object, a `shown(bool)`, and a row in that table |
| `main/clock_page.c/.h` | the clock page, a Bauhaus poster: an analog clock after Max Bill's (drawn), the time and date, the month's calendar (weeks from Monday) with notes per day |
| `main/notes.c/.h` | the calendar's notes, in their own SQLite database: see Clock below |
| `main/time_sync.c/.h` | the time zone and SNTP: see Clock below |
| `main/weather.c/.h` | the forecast: `config.json` on the card, the four providers, the fetch task: see Weather below |
| `main/weather_page.c/.h`, `main/weather_icon.c/.h` | the weather page (the place's sky painted with the weather in it); its icons, drawn with LVGL shapes |
| `main/artwork.c/.h`, `main/artwork_page.c/.h` | the artwork of the day (an art book's spread): see Artwork below |
| `main/sun_moon.c/.h`, `main/sky_paint.c/.h`, `main/sky_view.c/.h`, `main/astro.c/.h` | the Sun and Moon page, the painted sky (shared with the Weather page), and the astronomy under it: see Sun and Moon below |
| `main/generative.c/.h`, `main/gen_art.c/.h` | the Generative page: see Generative below |
| `main/music.c/.h`, `main/music_synth.c/.h` | the Generative page's music: see Generative below |
| `main/album_wall.c/.h`, `main/albums.c/.h` | the Album Wall: a Deezer user's favourite albums. See Album Wall below |
| `main/gallery.c/.h`, `main/thumb.c/.h` | the Gallery page (a salon hang); its thumbnails. See Gallery below |
| `main/index.c/.h`, `components/sqlite3/` | the picture index: SQLite on the card. See Index below |
| `main/settings.c/.h`, `main/settings_page.c/.h`, `main/night.c/.h` | the settings (NVS), their page (a blueprint), and night mode: see Settings below |
| `main/now_showing.c/.h`, `main/glow.c/.h` | "?" top right of the slideshow: the picture on show and Wikipedia's summary of it, on a glow of its colours; the slideshow's wall label: see Now Showing below |
| `main/diffuse.c/.h`, `main/diffuse_page.c/.h` | the drop left of "?": diffusion effects on the picture on show. See Diffuse below |
| `main/net.c/.h` | HTTPS GETs (into memory, or straight into a file; redirects followed; a connection kept for a run of calls to one server), JSON path helpers, cJSON in PSRAM |
| `main/fonts/` | Montserrat 16/20/24/32 with Latin-1, Latin Extended-A and typographic punctuation (`art_font_*`): the museum's names. LVGL's built-in fonts are ASCII only |
| `main/slideshow.c/.h` | the microSD picture slideshow: see Slideshow below |
| `main/transition.c/.h` | the change from one picture to the next: see Slideshow below |
| `main/brightness_ui.c/.h` | the backlight: a button bottom left opens a slider (folds away after 5 s untouched); the level is kept in NVS (`artshow`/`bright`) |
| `main/board.c/.h` | the board: power (LDO3 for the D-PHY), I2C, PWM backlight, the EK79007 MIPI-DSI panel (1024x600), GT911 touch, LVGL through esp_lvgl_port. `board_set_brightness(0..100)`, `board_i2c_bus()` |
| `main/board_pins.h` | every pin and panel value, proven on the bench board. Don't change them without a reason from the hardware |
| `main/idf_component.yml` | the component dependencies. It must stay in `main/`: a manifest at the project root is silently ignored |
| `sdkconfig.defaults` | the committed config. `sdkconfig` is generated and gitignored |
| `partitions.csv` | NVS at 0x9000 (as in IDF's single-app-large table), the app at 4 MB |
| `main/wifi.c/.h`, `main/wifi_ui.c`, `main/Kconfig.projbuild` | Wi-Fi through the C6: see Wi-Fi below |
| `main/audio.c/.h` | the speaker: see Audio below |
| `main/Kconfig.projbuild` | also `idf.py menuconfig` > ArtShow slideshow: the first-boot defaults of seconds per picture, transition length, varied transitions, shuffle, fill (crop); the Settings page changes them |

## Build and flash

```sh
idf.py set-target esp32p4 build     # once; after that, build alone
/root/Scaricati/CYD/advance-p4-wiz/p4wiz.py --check .                # every sdkconfig.defaults line made it into sdkconfig?
idf.py -p /dev/ttyUSB3 -b 460800 flash monitor
```

- Run `idf.py` from an ESP-IDF 6.x terminal (`export.sh`, or EIM's `activate_idf_*.sh`).
- After editing `sdkconfig.defaults`, delete `sdkconfig` and rebuild, or the
  change is not applied. Then run `--check` again.
- Flash through the **UART** USB socket (CH340), at **460800** baud: 921600
  corrupted reads on this board. If it won't enter the bootloader, hold BOOT,
  press RESET, let go of BOOT.
- Flashing replaces whatever the board runs. Ask before flashing a board, and
  offer a backup first: `esptool.py -p /dev/ttyUSB3 -b 460800 read_flash 0 0x1000000 backup.bin`.

## Rules for working on this app

- **Chip revision:** **rev v1.x** (the bench board is v1.3): `sdkconfig.defaults` builds for it. A rev v1.x P4 needs
  `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` and `CONFIG_ESP32P4_REV_MIN_100=y`;
  an image built for rev >= 3 (IDF 6.x's default) builds fine but won't boot on
  it. Check a board with `esptool.py -p /dev/ttyUSB3 -b 460800 chip_id`.
- **The DSI clock:** on rev v1.x, `board.c` uses
  `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY`. IDF 6.1's plain `_DEFAULT` is
  XTAL, which only rev >= 3 has: `esp_lcd_new_dsi_bus` aborts at boot.
- **LVGL 9.6:** any LVGL call from outside the LVGL task holds
  `lvgl_port_lock(0)` (it waits forever; the lock is recursive). Use 9.6's API:
  the per-flag setters (`lv_obj_set_hidden`, `lv_obj_set_clickable`, ...),
  `LV_LABEL_LONG_MODE_*`; `lv_list` is deprecated, so build lists as a flex
  column. Fonts: Montserrat 14, 24 and 32 are enabled in `sdkconfig.defaults`;
  add a `CONFIG_LV_FONT_MONTSERRAT_<n>=y` line for others.
- The screen is 1024x600, landscape. The panel can't swap x/y.
- **No `[` or `]` in the project path:** LVGL's source glob breaks.
- **No codec:** the schematic shows an ES8311, but the board has none. The I2C
  bus holds only the GT911 (0x5d) and the STC8 helper (0x2f).
- Keep the build free of warnings, and grep the build log for "unknown kconfig
  symbol": it means a `sdkconfig.defaults` line was silently dropped.
- Don't write secrets into committed files (`sdkconfig.defaults`, sources).

## Harmless log lines

- `W gpio: conflict found for GPIO[42]`: the GT911 INT pin; touch works.
- `E lcd_panel: swap_xy is not supported by this panel`: esp_lvgl_port asks
  the EK79007; nothing breaks.

## Wi-Fi (esp_hosted 2.12.x)

The P4 has no radio: Wi-Fi runs on the onboard ESP32-C6, over 4-bit SDIO (CLK
18, CMD 19, D0..D3 17..14, C6 reset GPIO 32). esp_wifi_remote forwards the plain
`esp_wifi_*` API to it, so **every `esp_wifi_*` call is an RPC to the C6**.

- `main/wifi.c/.h`: one Wi-Fi task owns every esp_wifi call. Others post it a
  message (`wifi_scan`, `wifi_join`, `wifi_forget`) and get callbacks
  (`wifi_on_status`, the scan callback), which run in the Wi-Fi task: take the
  LVGL lock in them before touching the UI. `wifi_start()` returns at once.
  Add new Wi-Fi work as a message to that task, not as calls from elsewhere.
- At boot it scans once (for the log), then joins the network saved on screen
  (NVS namespace `p4wifi`), else the menuconfig one, and rejoins with a backoff
  (1, 2, 4 ... 30 s) after each drop. A network set on screen is saved once it
  gets an IP.
- `main/wifi_ui.c`: the status button (bottom right: the Wi-Fi icon only, white
  when connected, dim grey when not) and the setup panel on
  `lv_layer_top()`: scan list, password, Connect, Forget.
- `main/Kconfig.projbuild`: `idf.py menuconfig` > CrowPanel Wi-Fi sets a
  fallback network. It lands in `sdkconfig`, which is gitignored. **Never put
  the SSID or password in `sdkconfig.defaults`**: that file is committed.
- The password ends up as **plain text** in `build/*.bin` and in the board's
  flash, either way. Don't share or upload such builds or flash backups.
- A failed Wi-Fi step logs and turns Wi-Fi off; it never reboots the app. Keep
  it that way: no `ESP_ERROR_CHECK` in the Wi-Fi code.
- Disconnect reasons (`esp_wifi_types_generic.h`): 15 handshake timeout (often a
  wrong password), 201 no AP found, 202 auth failed, 8 we left.
- The `CONFIG_ESP_HOSTED_*` lines in `sdkconfig.defaults` are for esp_hosted
  2.12.x only. **esp_hosted 3.x renamed every option**: 2.x names under 3.x
  still build, but with the wrong SDIO pins and C6 reset pin, and the only sign
  is "unknown kconfig symbol" in the build log. After a config change, check
  that `sdkconfig` has D0 = 17 and reset = 32 (`p4wiz.py --check .` prints them).
- Elecrow ships the C6 with esp_hosted 2.12.3. esp_hosted 2.12.x matches it.
- esp_hosted warns that `CONFIG_FREERTOS_HZ` is 100 and it recommends 1000.
  Wi-Fi works at 100.

## Audio

- `main/audio.c/.h`: plain I2S into the speaker amps (BCLK 22, WS 21, DOUT 23),
  no codec. `audio_init()` after `board_init()`, then `audio_beep(hz, ms)`.
- The amps' enable, GPIO 30, is **active low** (10k pull-up: off by default).

## Slideshow

- `main/slideshow.c`: one task mounts the microSD card on `/sdcard`, lists every
  `.jpg`/`.jpeg` from the index (Index below: any folder, 4 deep, up to 2000;
  dot files skipped), decodes
  each with the P4's hardware JPEG engine into PSRAM, box-scales it into a
  1024x600 RGB565 buffer (`s_next`), and `transition.c` moves the one on show
  (`s_out`, the only `lv_image`) to it. The next picture loads into `s_next`
  during the countdown, once the transition is done with it, so it shows
  on time (and at once on a right tap; a left tap loads the previous one). Taps on the screen's left / right third:
  previous / next.
- The SD slot: SDMMC **slot 0**, 1-bit, CLK 43, CMD 44, D0 39 (Elecrow's P4 SD
  lesson), internal pull-ups, IO powered by LDO4 (`board.c` takes it at 3.3 V).
  esp_hosted has **slot 1** of the same SDMMC host for the C6: never deinit the
  host, only slot 0. D1..D3 stay unused: 40..42 are touch and LCD lines.
  **Set `host.slot = SDMMC_HOST_SLOT_0` by hand:** on the P4, `SDMMC_HOST_DEFAULT()`
  picks slot 1. A mount on slot 1 fails ("slot is not available"), its cleanup
  removes esp_hosted's slot, and esp_hosted asserts: the board reboots in a loop.
  **No card init while Wi-Fi runs:** a card init changes the controller's
  clock, and doing it while esp_hosted's link to the C6 is up (or coming up)
  times out (0x107) and breaks that link: esp_hosted logs "Unrecoverable host
  sdio state" and restarts the board. Reads once both are up are fine. So
  `app_main` mounts the card once, before `wifi_start()` (esp_hosted's link
  starts in `esp_wifi_init`). No remounts: with no card, no pictures, or a card
  that stops reading, the screen says so and a tap restarts the board.
  **A card left mid-read:** a reset of the board (the card keeps its power)
  in the middle of a read leaves the card holding D0; the first clock update
  then waits for ever and jams the whole host, Wi-Fi's slot too (no Wi-Fi that
  boot). `sd_unstick()` clocks the card by hand and sends it CMD0 before the
  mount: the log line "card D0 BUSY, after CMD0 idle" is that case, handled.
- `main/transition.c`: each change is one effect at random (never the same twice
  in a row): dissolve, iris, ink drop, brushstroke, mosaic, sand, blinds, clock.
  An effect is only a map: per 4x4 block, when it starts to turn (0..255). A
  20 ms timer in the LVGL task blends the blocks the front has reached from a
  snapshot of the old picture to the new (RGB565, 33 alpha steps; a block row's
  alphas first, then each pixel row streamed), and invalidates one area per
  20 px band (30 areas: LVGL keeps 32). Not LVGL objects per tile: hundreds of
  objects per change. New effect: a map function and a row in `s_fx`. Speed is PSRAM
  bandwidth (the panel's scan-out, LVGL's two framebuffers and three screen
  buffers share it): per frame ~18 ms blend + ~25 ms LVGL, 17-22 fps. The
  PPA draws the picture into the framebuffer (`CONFIG_LV_USE_PPA`, `_IMG`;
  it needs `CONFIG_LV_DRAW_BUF_ALIGN=64`, the cache line, and `s_out`
  64-byte aligned); the CPU copy was ~40 ms. Whole-screen redraws each
  frame were slower (7-12 fps) and fixed nothing.
  At the end it copies the new picture whole and redraws the whole screen:
  rounding on the way must never leave blocks short of fully turned (that
  left ghosts of the old picture). With LVGL's direct mode and two
  framebuffers, the off-screen one lags until the next refresh (LVGL syncs it
  then): comparing it with the picture after a transition shows differences,
  and that's fine. `menuconfig` > ArtShow: the length (1000 ms), and dissolve-only.
- The wall label (`now_showing.c`, `slideshow_on_show`): after each change,
  a museum's cartel bottom left (right of the brightness button): a cream
  card with a dark rule down its left edge, the artist and the title from
  the file name, faded in 1.3 s after the change (past the transition),
  held 8 s, faded out. The fade sets each part's opacity, not the card's
  `opa` (that draws through a layer: a buffer the card's size in internal
  RAM). Not clickable: taps go on to the slideshow. Off in Settings
  (`g_settings.captions`); hidden under Now Showing and other pages.
- The slideshow is the menu's first page: the screen itself; its "?" button
  (Now Showing) only shows on it. Another page
  covers it and `slideshow_pause()` holds the next picture back (it still
  loads); back on the slideshow, it shows at once.
- The slideshow task runs at idle priority: a software decode doesn't yield for
  5-9 s, and at any higher priority core 1's idle task starves (task watchdog).
- Two decoders. The P4's JPEG engine when the full-size picture fits in
  `DEC_MAX` (8 MB of RGB565, ~4 MP): ~50 ms. Bigger ones (the card's 4096 px
  pictures): `esp_new_jpeg` in software, shrunk 1/2, 1/4 or 1/8 while decoding
  (to multiples of 8, rounded up: no less than 1/8), ~3-9 s plus ~1.5 s to read the file over 1-bit SD. The
  engine can't shrink, and a 4096x3000 picture at full size (24 MB) doesn't fit
  in PSRAM. Both do **baseline only**: progressive JPEGs are skipped (logged).
  EXIF rotation is ignored.
- File names: `CONFIG_FATFS_API_ENCODING_UTF_8` (accented names, not 8.3 ones).
- The SD clock is 20 MHz (`SD_FREQ_KHZ`); Elecrow's lesson uses 10 MHz if a card is flaky.

## Clock

- `main/time_sync.c`: `setenv("TZ")` from menuconfig > ArtShow clock (default
  `CET-1CEST,M3.5.0,M10.5.0/3`, Italy) and esp_netif's SNTP with the server
  there (`pool.ntp.org`). SNTP starts in `app_main` after `wifi_start` (which
  inits esp_netif) and again on each `IP_EVENT_STA_GOT_IP`: its first tries
  fail before there's an IP and it then waits long to retry (27 s, measured);
  restarted, the time comes ~3 s after the IP. It re-syncs every hour.
- `main/clock_page.c`: a Bauhaus poster. On a charcoal ground, flat
  shapes in the primaries drawn by the page's own draw event (a red disc
  behind the clock, a blue bar and a yellow disc, cream bars). A cream
  ground lost the corner buttons (styled for dark pages: a faint tint, a
  grey icon) and the grey text; black squares under them were tried and
  disliked; the clock after Max
  Bill's, drawn in its own draw event (bar markers, no numerals, square
  black hands, a red second hand with a disc), invalidated once a second;
  the time and the date (lowercase) in cream, on the red disc and past it. The calendar flat
  in a black frame, its day cells restyled by a `LV_EVENT_DRAW_TASK_ADDED`
  callback added after LVGL's own (today a red disc, a day with a note
  blue, weekday names and other months' days grey). Until the first sync
  the time reads `--:--`, the face has no hands and the calendar is hidden
  (the clock says 1970). A 200 ms LVGL timer only while shown; a new day
  resets the calendar to the current month (its arrows show others
  meanwhile). Test it from the LVGL task (an `lv_timer`), not from
  `app_main`: showing it reads the notes database, which overflows the
  main task's stack.
- Notes: tap a day on the calendar for its note (an editor over everything
  on `lv_layer_top`, with the keyboard; Save, Delete, Close). Days with a note
  are highlighted (`lv_calendar_set_highlighted_dates`, every month); today's
  note shows under the calendar. LVGL's highlight is a fill on the day's
  cell, and it draws no fill for a transparent cell: the cells are opaque in
  the calendar's colour instead of transparent.
- `main/notes.c`: `/sdcard/.artshow/notes.db`, `notes(day "2026-10-20", text,
  updated)`. Its own database, not the picture index's: that one is made
  anew when it can't be read, and notes are written, not found again. One
  that can't be read is renamed (`notes-unreadable-<time>.db`) and kept.
  `synchronous = FULL` (FAT, a reset board: see Index). It shares index.c's
  SQLite lock (`index_sqlite_lock`): SQLite is built for one thread, and
  the first database opened sets SQLite up (memory from PSRAM).
- LVGL takes its memory from the system heap (`CONFIG_LV_USE_CLIB_MALLOC`), not
  a 64 KB pool: drawing the clock page takes ~125 KB of internal RAM for a moment.

## Weather

- `main/weather.c`: the settings are in `/sdcard/config.json`, read once at
  boot (a changed card needs a restart anyway: no remounts). No file: a template
  is written there, and the page says to fill it in on a computer:
  ```json
  { "weather": { "provider": "openweathermap", "location": "Rome,IT", "place_name": "",
                 "units": "metric", "lang": "en", "refresh_minutes": 30,
                 "api_keys": { "openweathermap": "", "weatherapi": "", "weatherbit": "", "visualcrossing": "" } } }
  ```
  `provider`: `openweathermap`, `weatherapi`, `weatherbit` or `visualcrossing`.
  `location`: a city ("Rome,IT") or "lat,lon" (every provider takes it; set
  `place_name` then, or Visual Crossing shows the numbers). `units`: `metric`
  (°C, km/h) or `imperial` (°F, mph).
- Every provider with a key can be picked on the page (a dropdown left of the
  refresh button, shown with two keys or more). The pick is kept in NVS
  (`artshow`/`wxprov`) and wins over `provider` at the next boot; `provider`
  counts again if the picked one loses its key. `weather_config_load` reads
  NVS, so it runs after `nvs_init`. The page asks, the weather task switches
  (`weather_use_provider` only posts the wish).
- **The API keys stay on the card.** Never put one in a source file, and never
  log a URL: the key is in it.
- Each provider's answer becomes one `weather_t` (now + up to 7 days), so the
  page doesn't know which one it was:
  - OpenWeatherMap (free plan): `/data/2.5/weather` + `/data/2.5/forecast`
    (5 days in 3-hour steps, grouped into days in the place's time; a day's sky
    is the step nearest 13:00).
  - WeatherAPI: `forecast.json`, one call; 3 days on the free plan. It's the
    one with hours on the free plan: every hour of each day comes (~70 KB), and
    the next 24 fill `w->hour[]`, the page's hourly strip (a row to swipe
    sideways). The others leave `nhours` 0: the strip hides and the day cards
    move up. cJSON allocates in PSRAM (`cJSON_InitHooks`): that answer is ~3000
    nodes, ~200 KB of internal RAM otherwise.
  - Weatherbit: `current` + `forecast/daily`. Free: 50 calls a day, so it
    refreshes every 60 minutes at least.
  - Visual Crossing: one `timeline` call, only the fields shown (`elements=`),
    `iconSet=icons2` for thunder and showers.
  OpenWeatherMap's and Weatherbit's condition codes share one scheme (`code_kind`).
- HTTPS through `esp_http_client` with IDF's certificate bundle. JSON with
  `espressif/cjson` (IDF 6 has no `json` component). The task starts once Wi-Fi
  is up, refreshes every `refresh_minutes` (and on the page's refresh button,
  top right), and after a failure retries in 1, 2, 4 ... minutes. A failure
  keeps the last forecast on show, with the error in orange under the place.
- `main/weather_icon.c`: the icons are drawn in the draw event on a 100x100 grid
  (circles, rounded bars, lines), so they scale to any size and look the same
  for every provider. The moon's bite takes the color of the first opaque
  background above it (so the cards and the hours' band are opaque). Two
  sets of inks: bright (the hours' dark band) and a print's
  (`weather_icon_print`: vermilion sun, ochre moon, indigo clouds and rain;
  the paper cards).
- `main/weather_page.c`: a woodblock print, like the Sun and Moon page:
  the place's sky (`sky_view`, the same hills) with the weather now in it
  (`weather_sky`: the kind to cover, rain, snow, fog, storm; sky_paint.c
  greys the sky, thickens and darkens the clouds, draws rain as fine
  slanting lines, snow on the peaks, mist over the hills, a bolt).
  Painted when shown, each minute while shown, and when a forecast comes.
  On it: a paper cartouche top left (place, temperature, sky, details,
  "Updated"), the hours (WeatherAPI) and the days as paper cards with a
  seal-red rule: the hours' a row to swipe, the days' (116 x 204: their
  ~170 px of text and icon) between the bottom corner buttons. Free plans count calls: each boot fetches,
  and a day of test reboots used up Weatherbit's 50 (HTTP 429 till the
  next day).

## Artwork

- `main/artwork.c`: a painting a day from the Metropolitan Museum's open API
  (no key). `v1.1/search?isHighlight=true&hasImages=true&medium=Paintings`
  (~420 hits, paged by `offset`/`limit`; the old `v1/search` was retired on
  2026-10-01) picks the hit by the date hashed; `v1/objects/<id>` gives the
  text, `primaryImageSmall` (~600 px, baseline) to show and `primaryImage`
  (1.5-4 MB, up to ~4000 px, baseline) to save. Not public domain or no
  picture: another hit 97 on (the hits run by department). "Another" picks at
  random; the next day brings the day's back.
- The "about": the painting's English Wikipedia summary (Wikidata item >
  `enwiki` sitelink > `rest_v1/page/summary`); most have no page, then the
  artist's. The page says "From Wikipedia, about ...": its text is CC BY-SA.
- The Art Institute of Chicago's API was the first choice; its image server
  answers non-browsers with a Cloudflare challenge (403), so it can't work here.
- The picture: decoded with `esp_new_jpeg` (software; shrinks while decoding if
  big) and box-fitted into 536x420, rows 64-byte aligned for the PPA. Two
  buffers: one on show, one to fill.
- `main/artwork_page.c`: an art book open on a walnut table, drawn in the
  page's draw event (its shadow, the page block's edges, the spine's
  shade). The left page a plate: the painting on its own average colour,
  deep, with a faint keyline; the right page cream paper, the catalogue
  entry ("PLATE <day of the year>", a seal-red rule, the text scrolling);
  folios from the day of the year. The book ends at y 534: the corner
  buttons (styled for dark grounds) stay on the table.
- Save (top right): the full-size picture, streamed to
  `/sdcard/Artwork of the Day/<artist> - <title>.jpg` 16 KB at a time
  (`net_download`; ~300 KB/s, so 5-30 s, with a percentage on the page). Card
  writes while Wi-Fi runs are fine (measured); only card inits aren't. The
  slideshow lists the card at boot: saved ones show after a restart.
- TLS: mbedTLS allocates in PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`): the
  weather and artwork tasks connect at the same time at boot, and internal RAM
  fell to 91 KB free. `CONFIG_MBEDTLS_HARDWARE_ECDSA_VERIFY` is off: a rev v1.x
  P4 has no ECDSA peripheral (each connection logged an error, then verified in
  software anyway).

## Now Showing

- Not in the menu: a "?" button top right of the slideshow (only there:
  `main.c`'s `slideshow_shown` tells it) opens the page over the slideshow,
  and turns into an X that closes it. Another menu page closes it too.
- `main/now_showing.c`: the page takes the slideshow's picture on show
  (`slideshow_now()`: its path; `slideshow_screen()`: the 1024x600 buffer,
  box-filtered to 480x281 after `transition_finish()`), and the slideshow is
  paused under it. Title and artist come from the file name, "Artist -
  Title.jpg" (DailyArt's: Italian titles, ':' written '_').
- The page, a nocturne: its ground is the picture itself, `glow.c` (plain
  C, -O2): averaged to a 16 x 9 grid, stretched back up with a smoothstep
  between cells, darkened to 100/256, a vignette, a 4 x 4 ordered dither
  (RGB565 bands otherwise); 1024 x 600 in ~0.23 s in the LVGL task, once a
  picture (kept by path), into a 64-byte aligned PSRAM canvas (1.2 MB). On
  it the picture sharp with a light keyline and a 24 px shadow (40 px took
  ~7 KB more internal RAM while drawing), the text in warm cream.
- The lookup, in its own task: a search on `CONFIG_ARTSHOW_WIKI_LANG`'s
  Wikipedia (menuconfig > ArtShow now showing; "it") for "title artist"; the
  top hit counts if its short description says painting (dipinto, xilografia,
  ...) and names the artist, and isn't the artist's own page. Else the
  artist's page by name (a search if there's no page by that exact name:
  "James Dixon Innes" is "James Dickson Innes"), there and then on English
  Wikipedia. Measured on the card's pictures: the painting is rarely there
  (the titles are DailyArt's translations); the artist nearly always.
- Wikipedia rate-limits its search API hard (HTTP 429, for minutes, after a
  burst from one address: the board shares it with the computer). So: one
  search per lookup at most, page summaries by title (cached by Wikimedia,
  generous), the last 16 answers kept, and 429 says "busy, try again".
- A TLS connection to Wikimedia costs ~2 s (ECDSA in software on rev v1.x):
  a lookup keeps one open for its calls (`net_keep_t`): 0.3-0.9 s each after
  the first. A lookup is ~4-6 s.

## Diffuse

- `main/diffuse.c`: plain C (it renders on a computer too: test there on a
  1024x600 RGB565 file first). Each effect runs a slice at a time:
  - dithering (error diffusion): Newsprint (grey, Atkinson, black on paper),
    E-paper (six inks), Game Boy (four greens), Its 8 colours (k-means on
    4096 pixels); Floyd-Steinberg back and forth. ~1 s on the board.
  - 8-bit: 6 x 6 blocks (averaged, a little more saturated and
    contrasted), the picture's own 16 colours (k-means on the blocks:
    PICO-8's fixed 16 turned the sky lavender), a 4 x 4 Bayer offset, hard
    squares. 0.2 s.
  - Melt: Perona-Malik anisotropic diffusion, 48 passes (kappa 30, lambda
    0.24), channels interleaved in 12-bit fixed point, each edge's flux
    worked out once: 15 s (was 94 s with planes, three times the work, at -Og).
  - Coral: Gray-Scott reaction-diffusion on a grid a third the size, Q15
    integers, k from the picture's brightness (0.060 dark: dense, 0.0685
    light: sparse; a sweep showed 0.062 covers half), 3 x 3 seeds of v = 1
    (single cells or weaker seeds just die), 1000 steps: ~50 s. Float,
    16.16 and Q15 all took 57-67 s for 1200: it's the CPU (~0.5 us a cell
    a step), not memory. Redrawn when the last redraw took under a fifth
    of the time (130 ms each).
  - `diffuse.c` builds at -O2 (`main/CMakeLists.txt`): the app is -Og.
    The P4 measured: a float op ~6 cycles; a 1.2 MB PSRAM copy 28 ms.
- `main/diffuse_page.c`: a view over the slideshow (paused; its background
  thumbnails off), the picture copied in by the effect's task (after any
  run left over has stopped), a task on core 1 at priority 1: 100 ms
  slices and a tick back (at 100 Hz a tick is 10 ms: 25 ms slices lost a
  third). Save: JPEG 4:4:4 q92 into `/sdcard/Diffused/<name> - <effect>.jpg`
  and `slideshow_added`. Testing Save on the board puts files in the
  slideshow: remove them after (the Rescan drops them from the index).
- The page's look, a printmaker's ink drawer: the effects as chips in a
  dark tray along the bottom, each with a swatch drawn of the inks it
  prints with (Newsprint's two, E-paper's six, the Game Boy's four; the
  picture's own colours, a quadrant's average each, as stripes, 8-bit
  squares, Melt's wash in three gradients, Coral's spots); the chosen one
  lifted and outlined in cream, its progress a hairline (redrawn once a
  second); Save a seal-red chop; the status a dark capsule. While an
  effect runs the view is redrawn only above the tray (the strip under it
  at the end), so the tray and its chips aren't redrawn with it four times
  a second. Timings vary from run to run: Melt took 15.0-16.0 s with the
  old page and 15.2-17.9 s with this one (five runs), 8-bit 0.2 s or 1.5 s
  when the boot's network work overlaps.

## Sun and Moon

- `main/astro.c`: plain C (it builds on a computer too): Paul Schlyter's
  low-precision sun and moon (orbital elements, the moon's main
  perturbations), sidereal time, altitude. Rise and set: the altitude every
  10 minutes, each crossing refined by bisection (sun -0.833 degrees; golden
  hours -4 to +6; the moon: parallax less refraction and radius). The next
  full/new moon and solstice/equinox: the elongation or the sun's longitude
  crossing its target. Checked against PyEphem for Milan over a year: the
  sun's times within 6 s, the moon's rise/set within 47 s, its lit fraction
  within 0.4 points, the next phases within 9 min, the seasons within 7 min.
  Polar day and night (no rise, no set) and the southern hemisphere work.
- The place: `weather_place()`: config.json's "lat,lon", else the
  coordinates the weather provider last gave (all four do), kept in NVS
  (`artshow`/`wxlat`, `wxlon`, `wxplace`, micro-degrees): offline after one
  fetch, without a card even.
- `main/sky_paint.c`: plain C (paint it on a computer first: a 1024x600
  RGB565 file): the sky as it is, like a shin-hanga woodblock print.
  Looking south (north, south of the equator), 120 degrees of azimuth
  either side across the width (`astro_sun_pos`, `astro_moon_pos`: altitude
  and azimuth), the horizon at two thirds. A three-colour gradient from a
  table of the sun's altitude (night, blue hour, sunrise, golden hour,
  day), paper grain (a 64 x 64 tile; each colour's 16 grained versions
  packed once: no float per pixel), stars below -6 degrees, clouds (long
  streaks, other ones each day, lit from below near sunrise and sunset),
  the day's path of the sun dotted, the sun (glow and disc, behind the
  hills), the moon (phase, soft seas, earthshine at night), three ridges
  of hills (summed sines from the place; one pass a column: the frontmost
  ridge's colour from a table, a ridge's edge blended over the hill
  behind). ~5 ms on the computer, ~0.4 s on the board.
- `main/sky_view.c`: the canvas (1024x600, PSRAM, 64-byte aligned) and
  the painter task, shared by the Sun and Moon and Weather pages (one is
  shown at a time): a page makes its own image of the canvas and asks for
  a paint (the latest wish wins). The painter is on core 1 at idle
  priority: a picture doesn't yield (0.4 s, up to 4 s beside the artwork's
  decoder), and at priority 1 core 1's idle task starved (task watchdog).
  It paints in place: a minute's change is a few pixels, so a half-painted
  one never shows; switching pages shows the other's for ~1 s.
- `main/sun_moon.c`: the picture full screen, painted each minute. On it: the
  times of sunrise, noon (and its height) and sunset in small dark tabs
  on the horizon where they happen, a cartouche top right (the title,
  the place, the date; a seal-red border), and a caption on the near
  hill, four lines a side: the day's length, its change, the golden
  hours, the next solstice or equinox; a small moon (painted, up or not)
  with its phase, rise and set, the next full moon, the next new moon.
  Each caption label has a fixed one-line height: LVGL's DOTS mode only
  shortens a label of a fixed height, else it wraps over the line below.
  The day's numbers when shown on a new day (or a new place): ~0.5 s in
  the LVGL task (doubles in software: the P4's FPU is single precision).

## Settings

- `main/settings.c`: `g_settings` (seconds per picture, transition length,
  varied, shuffle, fill, night mode, hours and level), a versioned blob in NVS
  (`artshow`/`settings`; another size or version: the defaults), defaults from
  menuconfig. Written in the LVGL task (the page), read anywhere when needed:
  the slideshow per picture, transition.c per change. The folders switched
  off: `artshow`/`skipdirs`, "\nA\nB\n" (exclusions: a new folder plays).
- The slideshow: `step()` skips pictures in a folder that's off (all off:
  any), and when the shuffle setting flips it reorders at once (a shuffle,
  or names) and starts from the top. Fill applies from the next picture
  decoded: the one preloaded already keeps the old one.
- `main/night.c`: a 1 s LVGL timer. Night: set hours (crossing midnight
  works), or today's sunset to sunrise (astro.c, once a day, the weather's
  place). Dark once LVGL's inactive time (`lv_display_get_inactive_time`) is
  past 2 minutes; a touch lights it at the slider's level (the touch also
  does what it touched). Night level 0: the backlight off and
  `slideshow_sleep()` (no reading or decoding).
- "Backlight switching" (Settings, Night): the backlight's PWM frequency, 1
  to 30 kHz (`board_set_backlight_hz`, 30 kHz at boot until the setting
  applies), live. The screen flickered when dim (15 %), more on white areas,
  and much the same at 1-30 kHz: likely the MT9201 near its floor, not the
  frequency. 25 % is steady: `BL_STEADY_MIN` (board.h) is the floor of the
  brightness slider and the night level (which also has 0: off); a lower
  saved level is raised at load.
- `main/settings_page.c`: a blueprint. A cyanotype ground with a drafting
  grid (one 80 x 80 tile, tiled), chalk line work: numbered sections in
  spaced capitals over a rule, sliders drawn as rulers (a hairline, the
  part up to the value heavier, ticks; LVGL draws only the open knob, the
  main and indicator parts transparent), outlined switches, boxes and
  buttons (filled when chosen: set the checked state's colour, or the
  theme's blue shows), the sheet's border and a dash-dot centre line
  between the columns, a title block top right (the firmware's version).
- The page's About: Wi-Fi (`wifi_info`: esp_netif, no call to the C6), free
  memory, pictures, uptime, the firmware's version and build date.
- Artwork of the Day's Save keeps a full-size picture only if it's within
  `SLIDESHOW_MAX_JPEG` (12 MB: the slideshow reads a file whole), else the
  small one.

## Index

- `main/index.c`: the pictures in SQLite, `/sdcard/.artshow/index.db` (a dot
  folder: the walk skips it): `pictures(path, folder, size, mtime, width,
  height, status, note, added, seen)` and `meta`. The slideshow lists from it
  at boot (~20 ms) instead of walking the card; an empty index is walked into
  at once. The Settings page's Rescan walks the card again (87 pictures:
  0.3-0.9 s): new ones in, gone ones out, a changed one (size or date) tried
  again. A picture that can't be shown (not a JPEG, too big, progressive or
  damaged; not an I/O error) is marked (status 2, note: why) and left out
  until it changes. Its first good showing stores its size in pixels: no
  write per picture after that (the card's wear). Artwork of the Day's Save
  adds its picture at once (`slideshow_added`).
- `SQLITE_THREADSAFE=0`, and a recursive mutex in index.c makes each public
  call whole: the slideshow's task and the Gallery's worker share the one
  connection (a rescan holds it ~1 s). The slideshow's list changes only in
  its task (`slideshow_rescan`, `slideshow_added`: a note and a queue);
  others read it under `s_list_lock`. `s_files` holds MAX_FILES pointers from the
  start: it never moves.
- `components/sqlite3/`: SQLite 3.54.1's amalgamation, unchanged (README:
  the source and its hash; ~10 MB in git, ~520 KB in the app), with
  `sqlite3_esp.c`, a VFS on POSIX calls (after SQLite's `test_demovfs.c`):
  no locks (FAT has none; one task), no WAL, temp tables in memory, memory
  from PSRAM (`SQLITE_CONFIG_MALLOC`). Its warnings are off (`-w`), as LVGL's.
- `PRAGMA synchronous = FULL`: on FAT a file's size and clusters reach the
  card only when it's synced or closed, and the board is reset, never closed.
  With NORMAL the next boot read the database as "disk I/O error". A
  database that can't be read is replaced by a new one, then walked into:
  it's only an index.
- The time zone is set first thing in `app_main` (`time_sync_tz`): the index
  opens before the time sync starts, and its "scanned" time is local.

## Gallery

- `main/gallery.c`: a salon hang. A claret damask wall (one 56 x 96 tile,
  made at boot, tiled by LVGL as the page's background image), each
  picture in a gilt frame drawn ring by ring (a moulding's tones, lit from
  the top left) round the thumbnail's own shape, with a drop shadow, and
  an ivory wall label under it (title, artist, the star). 12 a page (4 x 3,
  between the bottom corner buttons; cells made once: an object
  per picture wouldn't fit internal RAM at 2000), sorted by artist then
  title; modes All, Favourites, Artists (21 artist buttons a page, then one
  artist's pictures), Can't show (with the index's reason). A pager top
  right. A worker task queries the index and decodes the thumbnails, filling
  the cells under the LVGL lock; a newer request stops an older one. A tap:
  `slideshow_jump()` and `menu_select(0)`: that picture next ("Loading ..."
  meanwhile: up to ~25 s if a thumbnail is being made, as decodes can't be
  stopped). The star (drawn: the fonts have none) toggles a favourite.
- Thumbnails (`thumb.c`): fitted into 176x112 in whole 16 px blocks (the
  decoder's output is then exact), esp_new_jpeg at quality 75 (~6 KB), in
  the index's `thumbs` table. Made when the slideshow shows a picture (from
  the screen buffer, as shown), and while it's paused (another page) or
  asleep: one picture at a time from the file, ~11 s each for the card's
  4096 px ones. The Gallery redraws when `index_thumbs_version()` moves.
- Favourites: `/sdcard/.artshow/favourites.txt` (one path per line) is the
  truth, written at each star; the index's `fav` copies it after each scan,
  so a rebuilt index (damaged, or a new layout) gets them back, and a picture
  off the card for a while keeps its star. The Settings page's "Only
  favourites" plays just those (none starred: all).
- The index's layout is schema 2 (artist, title, fav, thumbs): another makes
  it anew, which is safe because favourites live in their file.
- Settings blobs: new fields go at the end; an older, shorter blob fills the
  start and the defaults stay in the rest.
  **A new `bool` can land in the old struct's padding**: the size doesn't
  change, the old blob covers it, and it reads 0 (`captions` did: it's an
  `int32_t` now). Check that `sizeof(settings_t)` grows, or use an int32.

## Generative

- `main/gen_art.c`: six pieces painting into a 1024x600 RGB565 canvas a
  slice at a time, each deterministic from its seed (the caption's number):
  a flow field (2500 particles on two octaves of Perlin noise, faint alpha
  trails; ~30 s), a Clifford or De Jong attractor (points counted per pixel,
  a log tone map through the palette, redrawn each second; 30 M points or
  50 s of painting, ~65 s in all), Hitomezashi (random row and column bits,
  the regions two-coloured by parity across stitches; ~20 s), stained glass
  (Voronoi round jittered grid seeds: only the 3 x 3 grid cells around a
  pixel can hold its nearest two; lead where they're about as near, each
  pane lighter at its middle; ~15 s), Mondrian (a rectangle split and split
  again, 70 % of panes the lightest colour, the bars what's left; ~12 s),
  Truchet (two quarter-circle arcs per tile, turned at random, soft edges;
  ~15 s). An attractor's settings are tried first: a trial run must cover
  enough of a coarse grid, else other settings. Each piece marks the rows it
  painted (`gen_dirty`): the page redraws only those. Rendered on the
  computer before the board (the same file, a PPM).
- Speed, measured on the board (1 M each): `sinf` 6.3 s, a 4096-step table
  0.25 s (so `fsin`/`fcos`, 25x); random `++` in a PSRAM buffer 0.9 s (now
  the attractor's limit: ~0.65 M points/s, was 0.44 M); `logf` 0.35 us, so
  the tone map is a table for counts 0..max, made once per refresh.
- `main/generative.c`: the canvas full screen (an lv_image, 64-byte aligned
  for the PPA). The chrome after the 1960s computer-art shows: a wall label
  at the bottom (the palette as five paint chips, a tap recolours; the
  caption's title, PAINTING or FINISHED, the settings, the colours' source,
  the music), the buttons in one dark console capsule. Top left: the kind (all in turn, or
  one; `g_settings.gen_mode`). Top right: Music, Recolour (the same seed in the
  colours of a random painting: its Gallery thumbnail, `index_random_thumb`,
  not a 4096 px decode), Hold (a finished piece stays till Next;
  `g_settings.gen_hold`), Next, Save. A task on core 1 at priority 1 paints
  25 ms, then gives a tick back (the idle task, LVGL), redrawing at most 4
  times a second; only while the page is shown. A finished piece stays 90 s.
  The palette: the 5 commonest unlike colours of the slideshow's picture (4
  bits a channel; black bars skipped), darkest first and made darker (the
  ground). Save: esp_new_jpeg at quality 90, 4:4:4 (thin lines keep their
  colour), ~280 KB, into `/sdcard/Generated/`, and `slideshow_added`. While
  it's shown the slideshow's background thumbnails wait
  (`slideshow_background(false)`): the CPU and PSRAM are this page's.
- Music (`main/music_synth.c`, plain C: it renders WAVs on a computer too):
  each piece has its own, from the same kind, seed and colours. The kind
  sets the scale, tempo and sound (flow field: lydian, soft FM; attractor:
  aeolian bells; Hitomezashi: hirajoshi plucks; stained glass: dorian
  chords; Mondrian: brisk pentatonic, walking bass; Truchet: a looping
  riff), the middle colour's hue the key, the seed the tune. 2-operator FM
  voices (10), a pad on the bar's chord, a feedback echo, a low-pass, a soft
  clip. `main/music.c`: a task on core 0 (the painter is on core 1) renders
  16 ms at a time into the I2S DMA, which paces it: ~1.3 ms of CPU each.
  `audio.c`'s channel auto-clears (silence between writes, not a buzz).
  The speaker button (top right, `g_settings.gen_music`, on by default)
  mutes it; it plays only while the page is shown, fades over 0.3 s, then
  the amps go off. Night mode's dark (dimmed or off) silences it
  (`music_quiet`).

## Album Wall

- `main/albums.c`: Deezer's public API, no key: `/user/<id>/albums` lists a
  public profile's favourite albums (100 a page; 297 here: 3 calls, ~10 s);
  `/album/<id>` gives the label, genres and tracks (the Spotlight's album,
  fetched when shown). `/user/me` needs an OAuth token (a registered app):
  not used. The ID: `config.json` > `"deezer": { "user_id": "..." }` (on the
  card, not in the sources; digits only, it goes in a URL).
- The cache: `/sdcard/.artshow/albums.db` (its own database, made anew if it
  can't be read: nothing in it is the user's own) and the covers in
  `/sdcard/.artshow/covers/`: `<id>.jpg` (1000 px, ~100 KB, baseline) and
  `<id>_m.jpg` (250 px), downloaded once (~1.7 s an album). An album without
  artwork: Deezer's cover address redirects to its blank one (MD5 of
  nothing, d41d8...): a `<id>.none` marker, not asked again, and a plain
  square with the title. (esp_http_client didn't follow that relative 302
  on the board: not chased, the marker is the better answer anyway.)
- `main/album_wall.c`: a record room. Spotlight (the cover 440 px, a sleeve
  with its record half out behind it, drawn: grooves, a sheen, a label in
  the cover's average colour, which also tints the artist and the chosen
  tab; the page in that colour darkened, title, artist, year, tracks, label, genres, the track
  list; another album every 30 s or on a tap) and Wall (6 x 3 covers a page
  standing on walnut shelves drawn wall to wall, a tap spotlights one); Sync top right. A worker task: requests (an album,
  a page, a sync) first; a sync fetches the list once a boot (or on Sync),
  then the covers missing, breaking off for any request and going on after
  it, never fetching the list again for that. The automatic Spotlight waits
  while covers download (it would break them off every 30 s). The cache is
  closed when the page is hidden.
- The Spotlight's tabs: Tracks (Deezer), About, Credits, Reception: an
  album's English Wikipedia article (then `CONFIG_ARTSHOW_WIKI_LANG`'s): its
  intro, its Personnel/Credits section, its Reception/Critical reception
  section (critics' quotes, ratings in words), the text from one
  `prop=extracts&explaintext` call. Finding the page: Deezer's title without
  "(OJC Remaster)" or " - Remastered", in Wikipedia's case ("Art Pepper Meets
  the Rhythm Section": page names are case-sensitive past the first
  letter), plain, then "(<artist> album)", "(album)" (it: "(album
  <artist>)"), the summary's description must say album and the artist;
  one search only as the last resort (the rate-limited call). Cached in
  `albums.db`'s `wiki` table: a found page for good, "nothing" for 30 days
  (the Sync button forgets them all); no answer (429, an error, no Wi-Fi:
  anything but a 404) isn't cached. A tab touched holds the automatic
  Spotlight 3 minutes (reading).
- Internal RAM: each open file on the card holds a 4 KB cache in internal RAM
  (`CONFIG_FATFS_PER_FILE_CACHE`, aligned work buffers), and SQLite keeps a
  database and its journal open: so `max_files` is 9, the notes and album
  databases are open only while used, and the big static buffers (generative
  sine table and state, palette histogram, the Gallery's cells, the album
  wall's text) are in PSRAM. Measured during a sync: ~108-120 KB free (the
  firmware before the Album Wall: 91-119 KB).
- The network tasks: the artwork, Now Showing, Album Wall and Gallery
  workers have their stacks in PSRAM (`xTaskCreateWithCaps`,
  `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`; none writes flash: the
  weather task writes NVS, so its stack stays internal). With them
  internal (42 KB), the boot's overlapping TLS connections took free
  internal RAM down to 4 KB: TLS failed (`-0x008D`, PSA's insufficient
  memory) and an SD read couldn't get its DMA buffer. Now: ~96 KB free,
  ~38-45 KB at the lowest. The artwork, Now Showing, Album Wall and
  weather tasks run at idle priority: a handshake verifies in software
  (rev v1.x) for seconds, and at priority 3 core 1's idle task starved
  (task watchdog).
