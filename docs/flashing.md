# Flashing Thingino

The overview: dump the stock flash (twice), build a Thingino image with the profile from this repo, write it
with a CH341A and a SOIC-8 clip, boot, and from then on update over the network.

You need:

- A **CH341A** programmer with a **SOIC-8 test clip** (the ~€15 kits). Read step 1 before you clip it onto
  anything: the common black board puts 5 V on its data lines.
- A multimeter.
- A Linux machine (or VM with USB passthrough) for `flashrom` and the Thingino build.
- Optional: a 3.3 V USB-UART adapter for the console (`ttyS1`, 115200, three pads below the T31). I never needed
  it, but it's the way to debug a boot that doesn't come up.

## 0. Open it up

All the chips are on the main board; the flash chip (P25Q64, SOIC-8) sits right next to the lens module. You can
clip it in-circuit with the board unpowered, but the clip won't fit as it comes: trim its plastic on the lens side.
A couple of minutes with a sharp hobby knife does it. Be patient and careful, you're cutting right next to the
contacts.

## 1. Check (and fix) the programmer's voltage

The P25Q64 is a 3.3 V chip. Many black CH341A boards power the chip with 3.3 V but drive the data lines at 5 V.
With the programmer plugged into USB and nothing in the socket, measure against GND (pin 4 of the 25-series
socket):

- socket pin 8 (VCC): should be 3.3 V
- socket pin 1 (CS): should be about 3.3 V. **5 V here = don't clip it onto your board yet.**

The fix is the well-known 3.3 V mod: lift pin 28 (VCC) of the CH341 chip off the board, and wire both pin 28 and
pin 9 (V3) to the 3.3 V output of the board's AMS1117 regulator. With the programmer unplugged, check first that
pin 9 and pin 28 aren't shorted to each other. Afterwards, socket pin 1 should read 3.3 V. A programmer that's 3.3
V-safe out of the box (or a Raspberry Pi's SPI with flashrom's `linux_spi`) works too.

## 2. Dump the stock firmware, twice

```sh
flashrom -p ch341a_spi -r stock-1.bin
flashrom -p ch341a_spi -r stock-2.bin
sha256sum stock-1.bin stock-2.bin   # must match
```

flashrom may not know the P25Q64SH by name and fall back to its SFDP "generic" chip; that worked fine. Keep this
dump safe: it's your way back to stock, and it contains your Tuya keys and Wi-Fi password, so don't share it.

**Use native USB.** Passing the CH341A into WSL with usbipd works for reading, but writing crawled at ~0.65
MB/minute (every 256-byte page is several USB round trips over the network). The same write took ~10 minutes
with the programmer on a real Linux USB port (a VM with USB passthrough counts).

## 3. Build the image

```sh
git clone https://github.com/themactep/thingino-firmware
cd thingino-firmware
git checkout 24ec28c        # the commit this was tested on; newer usually works too

# from this repo:
cp -r /path/to/this-repo/thingino/configs /path/to/this-repo/thingino/user .
```

Now fill in your own values in `user/princess_feeder_t31l_sc2336_atbm6012bx/overlay/`:

| File | What to put in it |
|---|---|
| `etc/wpa_supplicant.conf` | your SSID and the hex `psk=` line from `wpa_passphrase "SSID" "password"`. 2.4 GHz only |
| `root/.ssh/authorized_keys` | your SSH public key(s) |
| `etc/hostname` | whatever you want to call it (default `pet-feeder`) |
| `etc/timezone`, `etc/TZ` | your time zone name and its POSIX TZ string (default Europe/Amsterdam) |
| `etc/feederd.conf` | your MQTT broker, credentials and topic prefix |

Build (the first build takes a while, it builds a whole toolchain):

```sh
CAMERA=princess_feeder_t31l_sc2336_atbm6012bx make
```

If you run builds from a Python virtualenv, deactivate it first: U-Boot's build needs `setuptools` from the system
Python and fails on `pylibfdt` otherwise. Thingino's `scripts/dep_check.sh` lists the build dependencies.

Then build feederd with the toolchain you just made and bake it into the image:

```sh
cd /path/to/this-repo/feederd
make THINGINO=/path/to/thingino-firmware overlay
cd /path/to/thingino-firmware
CAMERA=princess_feeder_t31l_sc2336_atbm6012bx make repack
```

The image ends up in `output/<branch>/princess_feeder_t31l_sc2336_atbm6012bx-3.10.14-uclibc/images/` as
`thingino-princess_feeder_t31l_sc2336_atbm6012bx.bin`: exactly 8 MB, the full chip.

Build gotchas:

- `make` does nothing if the `.bin` already exists. Overlay-only changes need `make repack`.
- A change to the profile's `thingino.json` needs `make cleanbuild`: per-package build directories keep stale
  copies otherwise.

## 4. Write it

Board unpowered, clip on the P25Q64:

```sh
sha256sum thingino-princess_feeder_t31l_sc2336_atbm6012bx.bin
flashrom -p ch341a_spi -w thingino-princess_feeder_t31l_sc2336_atbm6012bx.bin
```

Wait for `VERIFIED`. Unclip and power it up.

## 5. First boot

It boots in well under a minute. Thingino doesn't use the stock MAC address; it derives a locally administered
one (starting with `02:`), so look for your hostname in your router's DHCP leases, not the old MAC.

```sh
ssh root@<feeder-ip>
```

Log in with your key. Thingino's web UI is on port 80; set a proper root password there or with `passwd` (the
default is `root`).

Then two settings that don't survive a full reflash/OTA (they live in `/etc/raptor.conf` on the writable overlay):

```sh
# The assembled feeder is upside down
raptorctl config set image hflip 1
raptorctl config set image vflip 1
# Optional: microphone and speaker off (I don't use them)
raptorctl config set audio enabled false
raptorctl config set audio ao_enabled false
raptorctl config save
reboot
```

The live stream:

- `rtsp://thingino:thingino@<feeder-ip>:554/ch0` (main, 1920x1080)
- `rtsp://thingino:thingino@<feeder-ip>:554/ch1` (sub, 640x360)

Change that RTSP password in `/etc/raptor.conf` (`[rtsp]` section). Every boot logs `IMP_FrameSource_EnableChn(0) failed` followed by a channel rebuild. That's harmless; video
works.

## 6. Check feederd

```sh
logread | grep feederd
mosquitto_sub -h <broker> -t 'home/pet-feeder/#' -v
mosquitto_pub -h <broker> -t home/pet-feeder/feed -m 1
```

You should see `online`, `food`, `led`, `feed 0` and the totals, and one portion should drop.

## Updating

No more clips. From the Thingino tree:

```sh
CAMERA=princess_feeder_t31l_sc2336_atbm6012bx make ota IP=<feeder-ip>
```

That sends the full image over SSH and reboots (~2 minutes). A full OTA wipes the writable overlay: repeat the
raptor.conf settings above, and expect a new SSH host key (`ssh-keygen -R <feeder-ip>`). feederd's lifetime
portion count survives: without a state file it adopts the last retained `portions-total` from the broker.

For quick feederd changes there's `make install FEEDER=root@<feeder-ip>` in `feederd/`, which only copies the
binary.

## Day and night

The IR LEDs (GPIO 59) and the IR-cut filter (GPIOs 10/11) are in the profile, so night vision switches on by
itself. This board has no light sensor, so Thingino decides day/night from the image brightness. If it ever sticks in
night mode, force day mode in the web UI, or look at the `[ircut]` settings in `/etc/raptor.conf`.

## Back to stock

Clip on, `flashrom -p ch341a_spi -w stock-1.bin`.
