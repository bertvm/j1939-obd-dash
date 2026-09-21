# J1939 / OBD dash — Waveshare ESP32-S3-Touch-LCD-7 rev 1.2

ESP-IDF firmware: J1939 (250/500 kbps) and OBD-II (500 kbps) on the onboard CAN PHY, LVGL dash with EPA-style telltales, readable DTCs, proprietary CAN tables, TSC1 / DPF regen commands, NVS settings, and optional SD overlays/logging.

## Hardware

- Pull **CH422G EXIO5 (CAN_SEL) high** — firmware does this. GPIO19/20 are then CAN, not USB OTG.
- CAN H/L on the PH2.0 header. 120 Ω jumper is fitted by default.
- Power the board from 5 V. Use a DC-DC from 12/24 V vehicle power. Lab-test with a USB-CAN adapter first.
- Flash/log via the **UART Type-C** port (CH343), not the USB OTG port.

## Build

ESP-IDF 5.1+ (this tree was built with 5.1; 5.3/5.5 also match the Waveshare board docs):

```bash
. $HOME/esp/esp-idf-v5.5.2/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-* flash monitor
```

If USB is not enumerated, hold BOOT, plug UART USB, release BOOT.

## Profiles (Set tab)

| Profile | Bitrate | Decode |
|---|---|---|
| J1939 250k | 250 kbps | Standard PGNs + DM1 |
| J1939 500k | 500 kbps | Same |
| OBD-II 500k | 500 kbps | ISO-TP Mode 01/03/07 |
| J1939+OEM 250k | 250 kbps | J1939 + proprietary table |
| Proprietary | NVS bitrate | Table only |

Default is **listen-only**. Enable J1939 transmit or OBD requests explicitly, then Save.

## Engine commands

TSC1 is hold-to-run at 10 ms. Forced regen uses DPFC1 after a confirm dialog. Address claim (SA `0xF9`) runs when TX is enabled. Interlocks abort on speed / park / bus-off.

## SD card (optional)

Copy examples from `data/` to `/sdcard/can/`:

- `proprietary.json`
- `spn_names.json`
- `obd_codes.json`

Set tab: **SD reload DB**, **Toggle log** (`/sdcard/log/signals.csv`).
