# epaper-display

ESP32 firmware for a Waveshare 4.2-inch V2 e-paper panel. It provisions Wi-Fi
and an image endpoint through a captive portal, downloads a 400×300 1-bit raw
bitmap on each wake-up, displays it, then deep-sleeps until the next refresh.

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
`http://192.168.4.1`. Enter Wi-Fi credentials, the image server's IP address,
and a refresh interval from 1 minute to 7 days. Only the IP is needed — the
firmware appends `:35000/epaper-display/image` automatically. Hold the ESP32
BOOT button for three seconds while starting up to force the portal again.

Set **Rotate display 180 degrees** to `1` if the panel is mounted upside down.

The endpoint is requested with `w=400`, `h=300`, and `id=<MAC address>`. It
must return HTTP 200 and exactly 15,000 bytes: a 1-bit, MSB-first, row-major
bitmap with no row padding. Responses may use either `Content-Length` or HTTP
chunked transfer encoding.

The panel performs one full refresh after every five partial updates to reduce
e-ink ghosting.

## Chart server modes

`src/main.py` supports two operating modes:

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
