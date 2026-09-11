# denon-panel

Waveshare **ESP32-S3-Touch-LCD-4B** (4", 480×480, 86-box) wall panel.

Target: LVGL touch UI that controls a Denon AVR-3312CI directly (telnet/HTTP),
with Home Assistant connected for lights and other entities.

## Files

| File | Purpose |
|---|---|
| `panel-phase1.yaml` | **Current build, working.** HA link + two-way-synced switches for `light.living_two` / `light.test_hue_white` + local backlight slider. |
| `secrets.yaml` | Wi-Fi / API / OTA secrets. Git-ignored. |
| `REF-4b-voice-satellite.yaml` | Community reference config for this exact board (voice satellite) — source of the hardware bring-up (I2C, expander, display init, touch). Kept for pin/init reference. |
| `ha-panel-4-3b-60s-touch-wake.yaml` | A different board (Waveshare 4.3B, 800×480) — kept for reference, not part of this project. |

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
Either works for flashing; the CH343 one has been more reliable for
`esphome logs` in practice — but network logs (`--device panel-test.local`)
are the most reliable of all on this Wi-Fi.

## Build

```bash
export PATH="$HOME/.local/bin:$PATH"   # uv-installed esphome
cd ~/denon-panel
esphome run panel-phase1.yaml --device /dev/cu.usbmodemXXXXXXX   # USB, first flash
esphome run panel-phase1.yaml --device panel-test.local          # OTA after that
```

## Roadmap

- **Phase 1 (done):** HA connection, 2 light switches (two-way sync, absolute
  turn_on/turn_off so rapid taps don't desync over a slow Wi-Fi link), local
  backlight slider.
- **Phase 2:** custom component talking to the Denon AVR-3312CI directly over
  telnet (port 23, receiver pushes state) + HTTP `goform` API for commands.
- **Phase 3:** add rooms/lights/status widgets from HA as wanted.

## History note

This project spent a while chasing the wrong board's documentation — the
first debugging pass assumed a Waveshare 2.8B (I2C on GPIO15/7), which
doesn't match this board at all. The 4B's I2C is on GPIO47/48. A bit-bang
GPIO scan (probing candidate pin pairs directly) is what found it; see git
history / session notes if that technique is ever needed again on new
hardware.
