# epaper-display

ESP32 firmware for a Waveshare 4.2-inch V2 e-paper panel. It provisions Wi-Fi
through a captive portal, fetches quotes and draws the market chart on the
device itself on each wake-up (or pulls a pre-rendered 400×300 1-bit bitmap from
your own image server, if you prefer), then deep-sleeps until the next refresh.

<img width="1920" height="1080" alt="IMG_3356" src="https://github.com/user-attachments/assets/dac1687e-7988-471f-b88c-2f2b705c35dc" />

## Build and flash

Install [PlatformIO](https://platformio.org/) and connect an ESP32-WROOM-32E
devkit, then run:

```sh
pio run --target upload
pio device monitor
```

The default wiring and target board are in `platformio.ini` and `src/display.h`.
Adjust the pin definitions before flashing if your display is wired differently.

## First setup

On first boot, join the `epaper-display-Setup` Wi-Fi network and open
`http://192.168.4.1`. Enter Wi-Fi credentials, the refresh interval (1 minute to
7 days) and the chart source. Set **Chart source** to `1` to let the device fetch
quotes and draw the chart itself (the default, no server needed) or to `0` to
pull a rendered bitmap from your image server — in that case enter the server's
IP too, and the firmware appends `:35000/epaper-display/image` automatically.
Hold the ESP32 BOOT button for three seconds while starting up to force the
portal again.

Note: the device resets to factory defaults on every cold boot — a real
power-on (unplug/replug), the EN/RESET button, or a crash. Config and saved
Wi-Fi credentials are erased and the setup portal reopens, so re-enter them
after each power-up. Battery-powered deep-sleep refreshes do not trigger a
reset; only an actual power cycle does.

Set **Rotate display 180 degrees** to `1` if the panel is mounted upside down.
Set **Keep WiFi on between refreshes for USB power** to `1` when the device is
plugged into USB/mains: the firmware stays awake (WiFi modem sleep) between
refreshes, so 1-minute updates skip the reconnect delay. Leave it `0` for
battery use (deep sleep, WiFi off between updates).

The endpoint is requested with `w=400`, `h=300`, and `id=<MAC address>`. It
must return HTTP 200 and exactly 15,000 bytes: a 1-bit, MSB-first, row-major
bitmap with no row padding. Responses may use either `Content-Length` or HTTP
chunked transfer encoding. This applies to server mode only.

The panel performs one full refresh after every five partial updates to reduce
e-ink ghosting.

## On-device chart mode

With **Chart source = 1** (the default) no backend is involved. Every wake-up the
firmware:

1. syncs the clock over NTP and applies the `America/New_York` timezone;
2. picks the session that is open right now, using the same windows as
   `src/main.py`:

   | Segment   | Symbol      | ET window   | Chart title                     |
   |-----------|-------------|-------------|---------------------------------|
   | Premarket | `NQ=F`      | 00:00–09:30 | NASDAQ FUTURES                  |
   | Regular   | `^ndx`      | 09:30–17:40 | NASDAQ 100                      |
   | Evening   | `930955.SS` | 21:30–00:00 | CSI Dividend Low Volatility 100 |

3. downloads that session from Yahoo Finance —
   `https://query1.finance.yahoo.com/v8/finance/chart/<symbol>?range=1d&interval=2m`,
   a single HTTPS GET of roughly 13–17 KB — and takes the latest price and the
   official previous close straight from the response's `meta` block, which is
   what makes the percentage agree with Yahoo's own page;
4. renders the chart into a 400×300 1-bit framebuffer (`src/chart.cpp`) and
   pushes it to the panel.

While every market is closed, or when the quote fetch or the render fails, the
panel keeps the last image rather than showing an error page — the behaviour the
server mode had.

Notes:

- The TLS connection is **not** certificate-validated (`setInsecure()`): the
  quotes are public data, and skipping validation keeps the firmware working
  when the CDN rotates its CA.
- The layout mirrors the matplotlib figure `src/main.py` renders, with the
  gradient reproduced through ordered (Bayer) dithering. Text uses the bundled
  FreeSansBold GLCD fonts, so there are no CJK glyphs.
- PlatformIO reports ~1.09 MB flash (83% of the default 1.25 MB app partition)
  and ~64 KB of static RAM; drawing needs a 15 KB heap buffer on top.

## Chart server modes (optional)

With **Chart source = 0** the device pulls a rendered bitmap instead.
`src/main.py` draws it with matplotlib and supports two operating modes:

- **Service mode** exposes the raw 400×300 bitmap endpoint for this firmware
  to pull on its own refresh schedule:

  ```sh
  python src/main.py --mode service --host 0.0.0.0 --port 35000
  ```

- **Push mode** generates the active market chart and pushes it to a Zectrix
  device repeatedly. Configure credentials with environment variables and the
  cycle with `--interval` (in minutes):

  ```sh
  export ZECTRIX_API_KEY='your-api-key'
  export ZECTRIX_DEVICE_ID='AA:BB:CC:DD:EE:FF'
  python src/main.py --mode push --interval 15
  ```

  The default interval is 15 minutes and can also be set through
  `PUSH_INTERVAL_MINUTES`. Use `--once` when an external scheduler such as
  cron should run a single push cycle.
