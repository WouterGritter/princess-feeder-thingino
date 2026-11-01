# princess-feeder-thingino

Local-only firmware for the **Princess Automatic Pet Feeder Camera 4L** (model 01.610045.01.001), a white-label
Tuya pet feeder with a camera. No cloud, no app, no account:

- [Thingino](https://github.com/themactep/thingino-firmware) on the Ingenic T31L: RTSP
  video, night vision, web UI, SSH.
- **feederd**: a small C++ daemon that replaces the Tuya app's job of talking to the feeder's MCU, and exposes the
  feeder over MQTT. Feed N portions, see when food runs low, see button presses, control the LED, and count
  portions.
- A Home Assistant config for all of it.

The write-up of how this was figured out: [gritter.nl](https://gritter.nl/posts/princess-pet-feeder-thingino/).

> Flashing custom firmware means opening the device, clipping onto a flash chip and possibly bricking it. Dump
> your stock firmware first (twice) and keep it. You do this at your own risk.

## Is this your feeder?

Check the board before anything else ([docs/hardware.md](docs/hardware.md)): Ingenic **T31L**, SmartSens
**SC2336** sensor, AltoBeam **ATBM6012B-X** USB Wi-Fi, Puya **P25Q64** 8 MB flash, and a Nuvoton **N76E003** MCU
on the main board. Other brands that sell the same Tuya board (Tuya product ID `2mdzbct5uqf4wsjg`) should work
too; different chips need a different Thingino profile.

## What's in here

```
thingino/configs/cameras/princess_feeder_t31l_sc2336_atbm6012bx/   Thingino camera profile
thingino/user/princess_feeder_t31l_sc2336_atbm6012bx/overlay/      files baked into your image (fill in!)
feederd/                                                           the MCU <-> MQTT daemon
home-assistant/pet-feeder.yaml                                     HA entities
docs/flashing.md                                                   step-by-step install
docs/hardware.md                                                   chips, GPIOs, UARTs, flash layout
docs/mcu-protocol.md                                               the feeder MCU's serial protocol
docs/stock-shell.md                                                optional: a root shell on stock firmware
tools/mcu-decode.py                                                decode strace logs of the MCU UART
```

## Quick start

1. Read [docs/flashing.md](docs/flashing.md). The short version:
2. Make sure your CH341A doesn't put 5 V on the data lines (most black ones do; there's a mod).
3. Dump the stock flash twice and compare.
4. Clone Thingino, copy `thingino/configs` and `thingino/user` from this repo into it.
5. Put **your own** values in `thingino/user/princess_feeder_t31l_sc2336_atbm6012bx/overlay/`:
   - `etc/wpa_supplicant.conf`: your SSID + `wpa_passphrase` output (2.4 GHz)
   - `root/.ssh/authorized_keys`: your public key
   - `etc/hostname`, `etc/timezone`, `etc/TZ`
   - `etc/feederd.conf`: your MQTT broker and topic prefix
6. `CAMERA=princess_feeder_t31l_sc2336_atbm6012bx make`, then `make THINGINO=... overlay` in `feederd/`, then
   `make repack` in Thingino.
7. Write the image with flashrom, boot, SSH in, flip the image (it's mounted upside down).
8. Add `home-assistant/pet-feeder.yaml` to Home Assistant.

## feederd

feederd plays the Tuya Wi-Fi module for the MCU on `/dev/ttyS0` (9600 8N1): it answers the heartbeat and time
requests, sends feed commands, and turns the MCU's reports into MQTT. Single-threaded, one `poll()` loop, ~130 KB.

### MQTT topics

All under `root_topic` (default `home/pet-feeder`). Every topic is two-way: feederd publishes the real state
(retained) and listens on the same topic for changes. Its own publishes are recognised and skipped, retained
messages replayed on subscribe are never treated as commands, and a write that isn't valid (or to a read-only
topic) gets the real state published back.

| Topic | Values | Writable |
|---|---|---|
| `feed` | `0` idle, `N` while dispensing N. Write `1`..`max_portions` to feed. A button on the device shows `1` then `0` | yes |
| `led` | `idle`, `off`, `on`, `slow_blink`, `fast_blink`. `idle` = config `led_idle` (also the startup state). The MCU's food-low blink always wins | yes |
| `food` | `ok` / `low` (hopper IR sensor) | no |
| `aux-button` | `idle`, `pressed` (short press: `pressed` then `idle`), `held` (while held). The button with the microphone icon | no |
| `last-feed` | `{"requested":5,"dispensed":5,"source":"mqtt\|button","at":"<ISO 8601>"}`, plus `"error":"timeout\|short"` on failures | no |
| `portions-total` | lifetime portions dispensed | no |
| `portions-today` | portions since `day_reset_hour` (default 04:00, so after-midnight snacks count as yesterday) | no |
| `online` | `online` / `offline` (last will) | no |

A feed while another one is running is refused. A feed times out after 15 s + 3 s per portion.

### Config

`/etc/feederd.conf`, `key=value`, unknown keys are an error. See the
[example](thingino/user/princess_feeder_t31l_sc2336_atbm6012bx/overlay/etc/feederd.conf).

| Key | Default | |
|---|---|---|
| `mqtt_host` | (required) | broker hostname or IP |
| `mqtt_port` | `1883` | |
| `mqtt_user`, `mqtt_pass` | empty | empty = no authentication |
| `mqtt_client_id` | `pet-feeder` | |
| `root_topic` | `home/pet-feeder` | |
| `max_portions` | `20` | per MQTT feed, 1..50 |
| `led_idle` | `off` | `off`, `on`, `slow_blink`, `fast_blink` |
| `day_reset_hour` | `4` | local hour at which `portions-today` resets |
| `serial` | `/dev/ttyS0` | |
| `state_file` | `/etc/feederd.state` | lifetime + daily totals, written only on a feed and at the daily reset |

Logs: `logread | grep feederd`. Run it by hand with `feederd -c /etc/feederd.conf -f -v` (stop the service
first: `/etc/init.d/S96feederd stop`). The init script restarts it if it ever exits, and says so in the log.

## Home Assistant

feederd doesn't do MQTT discovery. [home-assistant/pet-feeder.yaml](home-assistant/pet-feeder.yaml) defines a
"Pet Feeder" device with: a Feed number (0-20), a Feed 1 Portion button, an LED select, Food Low and Feeding
binary sensors, and Portions Today, Portions Total, Last Feed and Aux Button sensors. Replace `home/pet-feeder`
if you changed `root_topic`.

For video, add the RTSP stream to your NVR or HA's camera integration:
`rtsp://thingino:thingino@<feeder-ip>:554/ch0` (change that password).

## Not done / not supported

- **Schedules:** not decoded. The MCU has its own RTC, an EEPROM and room for 3x D cells, and the host sends it the
  time every ~30 s, so it may run a schedule by itself (also during a power cut, with the T31 and feederd off). The
  stock app also keeps a schedule on the camera side (`/conf/tuya/gv_feeder_schedule.json`, cloud DP 236 "meal
  plan"), and the MCU reports no schedule DP when queried. Clear any schedule in the Tuya app before flashing, and
  schedule feeds from Home Assistant instead. Pointers in [docs/mcu-protocol.md](docs/mcu-protocol.md#schedules).
- **Speaker and microphone:** mostly untested on Thingino (I left the mic out and disconnected the speaker). The
  speaker played a test sound once with GPIO 50 (amp enable) driven high by hand; with `spk_gpio=50` set on
  `audio.ko`, the driver held the pin low and it stayed silent. The mic was never tried.
- There's no light sensor on this board, so day/night switching goes by image brightness. See
  [docs/flashing.md](docs/flashing.md#day-and-night).
