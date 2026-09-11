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
- Status: poll `GET /goform/formZone2_Zone2XmlStatus.xml` (Zone2/"Kitchen"),
  on a plain 2s timer only (`interval:` -> a shared `script:`). It runs
  continuously in the background regardless of which page is showing, so by
  the time you switch to the Denon page its widgets are already at most ~2s
  stale - no need to poll again on page-select or on wake. Earlier revisions
  *did* poll on page `on_load` and on wake-from-idle, which seemed like a
  nice way to guarantee freshness - but `http_request.get` blocks the whole
  event loop (touch + display included) until it returns, so those extra
  polls turned page navigation and waking the screen into a multi-second
  freeze. Removed; the periodic-only version has none of that.
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
- **This receiver actually supports real push updates via UPnP/DLNA eventing**
  (confirmed via SSDP: it advertises `RenderingControl`, `AVTransport`, and a
  Denon-proprietary `X_WholeHomeAudio` service, each with a GENA `eventSubURL`
  — this is how the official iPhone app gets near-instant updates with no
  polling at all). The panel doesn't use this — it would need to run its own
  tiny embedded HTTP server to receive `NOTIFY` callbacks, handle the
  `SUBSCRIBE`/renew handshake, and reverse-engineer `X_WholeHomeAudio`'s
  (undocumented) event XML - real engineering, not something ESPHome has a
  component for. Noted as a "someday" idea; polling is what's actually built.

## Screen power management

- Idle timeout: 45s of no touch dims the backlight to 12%.
- **Denon polling pauses while idle**, but only once the screen has been
  dimmed *and* Zone2 has been off continuously for 30+ seconds (`interval:`
  lambda + `denon_pause_candidate_ms`/`denon_polling_paused` globals). A
  brief off-then-back-on never crosses that threshold, so it never touches
  the pause state. If Zone2 is on, polling never pauses regardless of screen
  state — a remote/app change is already reflected the instant you look at
  the panel, no catch-up delay.
- **Wake resets stale display to placeholders**: if polling had been paused
  (screen could've been dark for hours), waking blanks the four Denon labels
  to `"Kitchen: --"` / `"Input: --"` / `"-- dB"` / `"Mute: --"` instead of
  showing a possibly very-wrong old snapshot; the next poll (≤2s later)
  fills them back in. This logic lives once in the `wake_panel` script, used
  by both wake triggers below, so they can't drift out of sync.
- **Two wake triggers**: touch, and picking the panel up. The QMI8658 IMU
  (`motion: platform: qmi8658`, same chip used for the `X_WholeHomeAudio`-
  unrelated onboard sensors) is polled every 100ms; a pickup is detected as
  4+ *consecutive* samples with combined acceleration deviating >0.20g from
  resting (1g) — a tap is a single-sample spike that can't sustain that long,
  a real pickup easily does. 3s cooldown after firing. **These thresholds are
  an untested starting guess**, not tuned against the real hardware/mount —
  expect to adjust the constants in the `on_boot: priority: -100` lambda
  after testing (too sensitive: raise `THRESH` or `NEED_SAMPLES`; not
  sensitive enough: lower them).

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
- **Phase 4 (idea, bigger lift):** replace HTTP polling with real UPnP/GENA
  event subscriptions (see above) for instant, zero-poll updates.

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
