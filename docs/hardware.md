# Hardware

Everything here is from one unit: the Princess "Automatic Pet Feeder Camera 4L", model 01.610045.01.001
(batch T06266). Princess is an EU brand, so there is no FCC ID with internal photos. Inside it's a white-label
Tuya pet camera (Tuya product ID `2mdzbct5uqf4wsjg`), so other brands may sell the same board. **Check your own
chips before flashing anything**; a different SoC variant, sensor or Wi-Fi chip needs a different profile.

## The boards

Everything that matters is on **one main board**: SoC, sensor, Wi-Fi, flash and the feeder MCU. A small daughter
board on the white plastic only has a ribbon cable in, connectors out, a few transistors and resistors, and the
USB-C power input.

| Part | Chip | Notes |
|---|---|---|
| SoC | Ingenic **T31L** | 64 MB RAM, MIPS XBurst1 |
| Image sensor | SmartSens **SC2336** | I2C 0x30, 1920x1080, mounted upside down (`shvflip=1`) |
| Wi-Fi | AltoBeam **ATBM6012B-X** | over USB (ID `007a:888b`), 2.4 GHz only |
| Flash | Puya **P25Q64SH** | 8 MB SPI NOR, SOIC-8, right next to the lens module |
| MCU | Nuvoton **N76E003AT20** | 8051, TSSOP-20. Runs the motor, buttons, LED and food sensor |
| Motor driver | LANKE **LK6208** | brushed DC, forward/reverse/brake |
| RTC | **DS1307** | keeps the schedule clock on batteries |
| EEPROM | **P24C02A** | 256 bytes, MCU settings/meal plan |

The motor turns until a limit switch clicks once per portion; the MCU counts the clicks itself. An IR LED +
receiver pair looks across the hopper: blocked beam = food present.

The 5 through-holes next to the N76E003 are most likely its ICP programming header. The pads labelled 红外+/红外-
are the IR LEDs, 光敏+/光敏- is a photoresistor footprint that is **not populated** on this model.

## Checking your SoC variant

Thingino's `soc` script needs `printf`, which the stock BusyBox lacks. From a stock shell (see
[stock-shell.md](stock-shell.md)) you can read the IDs directly:

```sh
devmem 0x1300002C   # 0x10031003 on ours = T31 family
devmem 0x13540238   # 0x3333.... on ours = T31L
```

The sensor shows up in `dmesg` on the stock firmware (`sc2336 ... 0x30 i2c0`). On Thingino, `dmesg | grep -i
sc2336` and `lsusb` (Wi-Fi) tell you the same.

## T31 GPIOs

| GPIO | Function |
|---|---|
| 10 / 11 | IR-cut filter coil: pulse 10 = day (filter in), 11 = night |
| 59 | IR LEDs (850 nm), active high |
| 49 | SD card detect |
| 50 | speaker amplifier enable (high = on), only if you want audio |
| 18 | sensor reset (handled by the driver) |

No Wi-Fi power/reset GPIO is needed: the ATBM chip enumerates on USB by itself.

**Not** on T31 GPIOs: both feed buttons, the "mic" button and the front LED. Those all belong to the MCU, see
[mcu-protocol.md](mcu-protocol.md).

Finding the IR pins took two senses: the 850 nm LEDs give a faint red glow you can just see, and a phone camera
shows them bright; the IR-cut coil makes an audible tick when pulsed.

## UARTs

| UART | Device | Use |
|---|---|---|
| uart0 | `/dev/ttyS0` | the MCU link, 9600 8N1. Must be enabled in the Thingino profile |
| uart1 | `/dev/ttyS1` | console, 115200 8N1. Three pads just below the T31 (untested by me) |

## Stock flash layout

From the stock U-Boot environment (`mtdparts`), for reference only:

| Offset | Size | Contents |
|---|---|---|
| 0x000000 | 256 KB | U-Boot |
| 0x040000 | 1792 KB | kernel |
| 0x200000 | 960 KB | rootfs (squashfs, xz, 64 KB blocks) |
| 0x2F0000 | 4800 KB | app (squashfs: `/app/bin/ipc`, the Tuya app) |
| 0x7A0000 | 384 KB | config (jffs2: Tuya keys, Wi-Fi, schedule) |

The config partition holds **your** device's Tuya local key and your Wi-Fi password. Keep your dump private.
