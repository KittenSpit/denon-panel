# denon-panel

Waveshare **ESP32-S3-Touch-LCD-4B** (4", 480×480, 86-box) wall panel, mounted
in the Kitchen. LVGL touch UI that:

- Connects to Home Assistant for two lights (two-way synced switches)
- Talks **directly** to a Denon AVR-3312CI's Zone2 ("Kitchen") over HTTP —
  no Home Assistant in that path at all
- Controls its own backlight locally

## Files

| File | Purpose |
|---|---|
| `panel-phase1.yaml` | **Current build, working.** HA light switches + backlight slider + a Denon (Zone2/Kitchen) control page: power, mute, volume ±, source select, live status. Denon page is the default/boot page. |
| `denon_http.h` | Tiny C++ helper (`denon_xml_extract`) for pulling `<Tag><value>` fields out of the Denon's goform status XML. Included via `esphome: includes:`. |
| `secrets.yaml` | Wi-Fi / API / OTA secrets. **Git-ignored** — copy `secrets.yaml.example` to make your own. |
| `secrets.yaml.example` | Template listing the required secret keys. |
| `REF-4b-voice-satellite.yaml` | Community reference config for this exact board (a voice-assistant satellite project) — source of the hardware bring-up (I2C, expander, display init, touch). Kept for pin/init reference. |
| `ha-panel-4-3b-60s-touch-wake.yaml` | A *different* board (Waveshare 4.3B, 800×480) — kept for reference only, not part of this project. |

## Hardware bring-up (this board)

- I2C: SDA GPIO47 / SCL GPIO48 @ 100kHz
- TCA9554 IO expander @ 0x20 — LCD reset/CS/etc. Also on the bus: GT911 touch
  (0x14, no reset/int pins wired), AXP2101 PMIC (0x34), PCF85063 RTC (0x51),
  QMI8658 IMU (0x6B), ES8311/ES7210 audio (0x18/0x40, unused here)
- Display: `mipi_rgb`, ST7701S, 480×480, 16-bit RGB. ST7701 init is
  **bit-banged through the expander** in an `on_boot` lambda (ESPHome's
  SPI-over-expander path is buggy on this board) — see `panel-phase1.yaml`
- Backlight: GPIO4 LEDC, inverted

Two USB ports on the board: native ESP32-S3 USB and a CH343 UART bridge.
Either works for flashing; network logs (`esphome logs ... --device
panel-test.local`) have been the most reliable way to see runtime output.

## Denon AVR-3312CI control (direct, no HA)

- **Telnet (port 23) is not available on this receiver/firmware** — confirmed
  refused on a live probe. Everything below is plain HTTP instead.
- Status: poll `GET /goform/formZone2_Zone2XmlStatus.xml` (Zone2/"Kitchen").
  No push, so this is on a timer (every 2s) plus on-demand (page shown,
  screen woken while already on the page) via a shared `script:`.
- Commands: `GET /goform/formiPhoneAppDirect.xml?<CMD>` for power
  (`Z2ON`/`Z2OFF`), volume (`Z2UP`/`Z2DOWN`, **1.0 dB per step** — verified
  live, not the 0.5 dB you'd guess from the Main Zone docs), and source
  (`Z2<SOURCE>`, e.g. `Z2CD`).
- **Mute is the one exception**: `Z2MUON`/`Z2MUOFF` do *not* actually flip
  this receiver's mute flag. Use the "web UI" style endpoint instead:
  `GET /MainZone/index.put.asp?cmd0=PutVolumeMute/ON&ZoneName=ZONE2` (and
  `/OFF`) — verified live, confirmed against the status XML's `<Mute>` field.
- The `<Mute>` field in the status XML *is* reliable once set correctly, but
  the panel adds a 3s grace period after any local tap before trusting a
  poll again — a poll landing right after a tap was catching the receiver
  mid-update and flashing the display back to the old state.
- `http_request:` is configured with a short (2s) timeout and
  `follow_redirects: false`, since `http_request.get` blocks the whole
  event loop (touch + display included) while a request is in flight — worth
  keeping in mind if you see the UI freeze after a command.

## Build

```bash
export PATH="$HOME/.local/bin:$PATH"   # uv-installed esphome
cd denon-panel
cp secrets.yaml.example secrets.yaml   # then fill in real values
esphome run panel-phase1.yaml --device /dev/cu.usbmodemXXXXXXX   # USB, first flash
esphome run panel-phase1.yaml --device panel-test.local          # OTA after that
```

## Roadmap

- **Phase 1 (done):** HA connection, 2 light switches (two-way sync, absolute
  turn_on/turn_off so rapid taps don't desync over a slow Wi-Fi link), local
  backlight slider.
- **Phase 2 (done):** direct Denon Zone2/Kitchen control over HTTP (see above).
- **Phase 3 (idea):** Main Zone ("Basement") and/or Zone3 ("Out-Bath")
  controls; more HA widgets (rooms/status) as wanted.

## History notes

- This project initially chased the wrong board's documentation — the first
  debugging pass assumed a Waveshare 2.8B (I2C on GPIO15/7), which doesn't
  match this board at all. The 4B's I2C is on GPIO47/48. A bit-bang GPIO scan
  (probing candidate pin pairs directly with raw `gpio_config`/manual
  clocking, no ESPHome `i2c:` component involved) is what found it — worth
  reusing if a future board's bus location is ever in doubt again.
- ESPHome's I²C driver version (legacy vs. the new `i2c-master` API shipped
  since ~2025.7) was investigated as a possible cause of the same symptom and
  ruled out — it was purely the wrong pins the whole time.
