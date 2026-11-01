# A root shell on the stock firmware (optional)

You don't need this to install Thingino. It's how the MCU protocol and the GPIOs were worked out, and it's
useful if you have a variant of this feeder that behaves differently. The stock root password is unknown, but the
app partition is a plain squashfs that runs a startup script, so you can add a telnet daemon to it.

You need your own dump of the flash first ([flashing.md](flashing.md), step 2). Work on a copy.

```sh
# Cut the app partition out of the dump (offset 0x2F0000, 4800 KB)
dd if=stock.bin of=app.img bs=1024 skip=$((0x2F0000 / 1024)) count=4800
unsquashfs -d app app.img

# Add a shell near the top of the startup script (the stock one even has '#telnetd &' commented out)
sed -i '4a /usr/sbin/telnetd -l /bin/sh &' app/init/app_init.sh

# Rebuild with the same settings as the original (check with: unsquashfs -s app.img)
mksquashfs app app-mod.img -comp xz -b 65536 -all-root -noappend

# Pad to the partition size with 0xFF and splice it back into a copy of the dump
size=$((4800 * 1024))
test "$(stat -c %s app-mod.img)" -le "$size" || echo "too big for the partition!"
python3 -c "import sys; d=open('app-mod.img','rb').read(); open('app-pad.img','wb').write(d + b'\xff' * ($size - len(d)))"
cp stock.bin telnet.bin
dd if=app-pad.img of=telnet.bin bs=1024 seek=$((0x2F0000 / 1024)) conv=notrunc
```

Before writing it, compare the two images: only bytes inside the app partition (0x2F0000 to 0x7A0000) should
differ. Write `telnet.bin` with flashrom like any other image, boot, and `telnet <feeder-ip>` gives you a root
shell without a login. The stock BusyBox is thin (no `printf`, `stty`, `pidof` or `hexdump`), so `echo -ne`
and `devmem` are your friends, and static MIPS binaries (`strace`, built with `-march=mips32r2 -static`) can be
`wget`ed into `/tmp`.

Two things to know:

- The Tuya app feeds a hardware watchdog. Killing or restarting `ipc` reboots the whole feeder within ~10 seconds.
- The stock firmware uploads clips to the cloud and wants Tuya's servers at boot. Block its WAN access while you
  experiment if you care about that, but note that the local live view in the Tuya app stops working without the
  cloud.

To go back, write your untouched dump.
