# ArtShow

An ESP-IDF 6.x app for the **Elecrow CrowPanel Advanced 7" / 10.1"** (ESP32-P4,
hardware V1.2), scaffolded by p4wiz on 2026-10-18 with: Wi-Fi (esp_hosted 2.12.x), audio.

The wizard and its full recipe live in the advance-p4-wiz repo
(https://github.com/HaikuZen/advance-p4-wiz; here: `/root/Scaricati/CYD/advance-p4-wiz`). Its
`docs/advance-p4-cookbook.md` explains every board value and pitfall below.

## Files

| path | what |
|---|---|
| `main/main.c` | `app_main`: `board_init()`, the card mount (before Wi-Fi) and `config.json`, NVS, then (under the LVGL lock) the slideshow, Now Showing (its "?"), the clock, weather and artwork pages, the Wi-Fi button, the brightness slider and the menu; then Wi-Fi, NTP and the weather and artwork tasks |
| `main/menu.c/.h` | the hamburger menu, top left: a table of pages (`menu_page_t` in `main.c`), one shown at a time. New page: a `create` that returns a hidden full-screen object, a `shown(bool)`, and a row in that table |
| `main/clock_page.c/.h` | the clock page: an analog clock (`lv_scale` with line hands), the digital time and date, the month's calendar (weeks from Monday) |
| `main/time_sync.c/.h` | the time zone and SNTP: see Clock below |
| `main/weather.c/.h` | the forecast: `config.json` on the card, the four providers, the fetch task: see Weather below |
| `main/weather_page.c/.h`, `main/weather_icon.c/.h` | the weather page; its icons, drawn with LVGL shapes |
| `main/artwork.c/.h`, `main/artwork_page.c/.h` | the artwork of the day: see Artwork below |
| `main/now_showing.c/.h` | "?" top right of the slideshow: the picture on show and Wikipedia's summary of it: see Now Showing below |
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
| `main/Kconfig.projbuild` | also `idf.py menuconfig` > ArtShow slideshow: seconds per picture, transition length, varied transitions, shuffle, fill (crop) |

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
  `.jpg`/`.jpeg` (any folder, 4 deep, up to 2000; dot files skipped), decodes
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
- `main/clock_page.c`: until the first sync the time reads `--:--:--` and the
  calendar is hidden (the clock says 1970). A 200 ms LVGL timer moves the
  hands, only while the page is shown; a new day resets the calendar to the
  current month (its arrows show others meanwhile).
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
  background above it.

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
  big) and box-fitted into 600x460, rows 64-byte aligned for the PPA. Two
  buffers: one on show, one to fill.
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
