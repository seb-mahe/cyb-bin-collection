# CYD Auckland bin collection display

Shows the next Auckland Council bin collection on an ESP32 "Cheap Yellow Display"
(ESP32-2432S028R). It shows the day, the date, how many days away it is, and which bins go out.
The data comes from an [aklapi](https://github.com/rusq/aklapi) server running on your LAN.

```
+--------------------------------+
|      Next bin collection       |
|           Tuesday              |
|         24 November            |
|  Tomorrow - bins out tonight!  |
|    [red]    [yellow]   [green] |
|   Rubbish  Recycling  Food     |
| 500 Queen Street, Auckland     |
| Updated Mon 23 Nov 06:00       |
+--------------------------------+
```

## 1. Run aklapi on your Linux machine

With Docker:

```sh
git clone https://github.com/rusq/aklapi.git
cd aklapi
docker compose up -d          # listens on port 8080
```

Or with Go (1.21+):

```sh
go install github.com/rusq/aklapi/cmd/aklapi@latest
~/go/bin/aklapi -port 8080
```

Test it. Use your own address:

```sh
curl "http://localhost:8080/api/v1/rr?addr=500%20Queen%20Street"
# {"rubbish":"2026-11-24","recycle":"2026-11-24","foodscraps":"2026-11-24","address":"500 Queen Street, Auckland Central"}
```

Find the machine's LAN IP with `hostname -I`. The ESP32 needs that IP, not `localhost`.
If you use a firewall: `sudo ufw allow 8080/tcp`.

## 2. Configure

```sh
cp include/config.example.h include/config.h
```

Edit `include/config.h`: WiFi SSID and password, `AKLAPI_BASE_URL` (for example `http://192.168.1.50:8080`),
and your address. `config.h` is git-ignored.

## 3. Build and flash (VS Code + PlatformIO on Linux Mint)

1. Open this folder in VS Code. PlatformIO downloads the libraries automatically.
2. Linux USB setup, needed only once:
   - `sudo usermod -aG dialout $USER`, then log out and back in.
   - Install the [PlatformIO udev rules](https://docs.platformio.org/en/latest/core/installation/udev-rules.html).
   - The CYD uses a CH340 USB chip. If the port appears and then vanishes,
     `brltty` is grabbing it: `sudo apt remove brltty`.
3. Click **Upload** (→), then **Serial Monitor** (plug icon) to see the logs at 115200 baud.

## When it fetches

| Trigger | Why |
|---|---|
| Boot | Gets data straight away |
| Every Monday at `WEEKLY_REFRESH_HOUR` (default 06:00) | The weekly refresh you asked for |
| The shown collection day has passed | Gets the following week's date without waiting for Monday |
| **BOOT** button pressed | Manual refresh |
| After a failure | Retries every 15 minutes and keeps showing the old data |

The screen redraws at midnight so "in N days" stays correct.
The red LED on the back lights the day before and on collection day.

## Display troubleshooting

CYD boards come in several variants. If the screen looks wrong, edit `build_flags` in `platformio.ini`:

- **Colours inverted** (black background): uncomment `-D TFT_INVERSION_ON=1`.
- **Red and blue swapped**: uncomment `-D TFT_RGB_ORDER=TFT_BGR`.
- **Screen stays white or blank**: you probably have the 2-USB-port variant (ST7789).
  Replace `-D ILI9341_2_DRIVER=1` with `-D ST7789_DRIVER=1` and add `-D TFT_INVERSION_ON=1`.
- **Image upside down**: change `tft.setRotation(1)` to `3` in `src/main.cpp`.
