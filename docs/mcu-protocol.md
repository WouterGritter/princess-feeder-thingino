# MCU protocol

The T31 never touches the motor. On the stock firmware, the Tuya app (`/app/bin/ipc`) talks to the Nuvoton
N76E003 MCU over `/dev/ttyS0` using the standard
[Tuya MCU serial protocol](https://developer.tuya.com/en/docs/iot/tuya-cloud-universal-serial-port-access-protocol?id=K9hhi0xxtn9cb),
with the camera playing the "Wi-Fi module" role. feederd takes over that role. This is what it needs to know.

## Framing

```
55 AA  ver  cmd  lenH lenL  data...  chk
```

- 9600 baud, 8N1.
- `chk` = sum of all preceding bytes (header included) mod 256.
- The host (T31) sends `ver = 0x13`; the MCU sends `ver = 0x10`.
- Unlike Tuya's documentation, where the module sends heartbeats and the MCU answers, this MCU asks and the host
  answers. Whatever the reason, answering is what keeps it happy.

## Commands used

| cmd | Direction | Meaning |
|---|---|---|
| `00` | MCU asks, host answers | Heartbeat. The MCU sends `55 aa 10 00 00 00 0f` every second; reply `55 aa 13 00 00 00 12`. Without replies, the MCU falls back to its "no network" LED blink. |
| `03` | host -> MCU | Network status, which the MCU turns into an LED pattern (table below). The MCU acks with `55 aa 10 03 00 01 00 13`. |
| `06` | host -> MCU | Set a DP. |
| `07` | MCU -> host | DP report. |
| `08` | host -> MCU | Query all DPs; the MCU reports each one with `07`. |
| `1c` | MCU asks, host answers | Local time, every ~30 s. Reply data: `01 YY MM DD hh mm ss wd` (YY = year - 2000, wd: Sunday = 1). `00` followed by zeros = time not known yet. |

The stock boot handshake (`01` product query, `02` work mode) was never captured (the stock app restarts the whole
device through its watchdog when you poke it), but it turns out not to matter: the MCU is happy with heartbeats, time
and DP commands alone.

## DPs

DP payload inside `06` / `07`: `dp type lenH lenL value...`. Type `02` = 4-byte big-endian integer, `01` = bool.

| DP | Hex | Type | Direction | Meaning |
|---|---|---|---|---|
| 203 | `CB` | value | set | **Feed N portions.** The MCU echoes `203 = N` |
| 215 | `D7` | value | report | **Portions dispensed**, sent once when done. Also sent unsolicited (`= 1`) after a feed button press |
| 214 | `D6` | bitmap | report | Faults. Bit 1 (`0x02`) = food low (hopper IR beam not blocked) |
| 240 | `F0` | bool | report | "Mic" button short press: `1` for ~0.6 s, then `0`. The MCU also blips it for ~40 ms on its own, so ignore pulses under ~250 ms |
| 241 | `F1` | bool | report | "Mic" button held: `1` while held, `0` on release |
| 201 | `C9` | raw | report | Always 0 here, unknown |
| 242 | `F2` | value | query | The stock app polls it ~2.5x/s. 0 at rest, a ~24 s countdown in ms after a feed. Unknown, not needed |

Feeding one portion, byte for byte:

```
55 aa 13 06 00 08  cb 02 00 04 00 00 00 01  f2
```

One portion takes ~0.6 s, five take ~8 s. The cloud-side DPs you may know from Tuya LAN tools (231 = feed, 238 =
report) are the Tuya app's own numbering; on the serial line they are 203 and 215.

Both physical feed buttons (the regular one, and the LED, which is itself a push button) make
the MCU dispense one portion by itself. The host only learns about it from the unsolicited `215 = 1`.

## LED

There is no LED DP. The LED is driven by the MCU from the network status byte (cmd `03`):

| Status byte | Pattern |
|---|---|
| `0`, `1`, `6` | slow blink, ~1 s |
| `2` | fast blink |
| `3` | double blink + pause... sometimes. Sometimes it goes dark and stays dark. **Don't use it** |
| `4`, `5` | steady on |
| `7` | **off** (out of range for the protocol, but clean and reliable) |

The food-low alarm (a double blink) is the MCU's own and overrides whatever you set; it clears by itself when food
is back. Re-send your status byte after that.

Custom patterns work by switching between `4` and `7`: the MCU acks each `03` within ~160 ms, so ~150 ms steps
give crisp blinks.

## Schedules

Not decoded. What is known:

- The MCU side has a DS1307 RTC, a 256-byte P24C02A EEPROM and a battery holder (3x D cells), and the host
  answers its time request (`1c`) every ~30 s. That fits an MCU that runs the schedule on its own, including on
  batteries with the T31 powered off.
- But the stock app also stores the schedule on the T31 (`/conf/tuya/gv_feeder_schedule.json`, handlers
  `DPID_MEAL_PLAN` / `handle_DP_feeder_schedule_new`, cloud DP 236), and a DP query (`08`) only returned 201, 214,
  240 and 241.
- None of the captures behind this document were taken while changing the schedule.

To find out: sniff the UART (below) while editing the schedule in the Tuya app, `i2cdump` the EEPROM before and
after (the stock BusyBox has it), and check whether a schedule fires on batteries with mains power removed. If the
MCU does keep a schedule, it would survive flashing Thingino, so clear it in the app first.

## How this was found

On the stock firmware, with a root shell (see [stock-shell.md](stock-shell.md)) and a static MIPS `strace`
attached to the Tuya app's UART file descriptor:

```sh
ps                      # note the PID of /app/bin/ipc (not the "/tmp/daemon @ipc" watchdog)
ls -l /proc/<pid>/fd    # which fd is /dev/ttyS0? 3 on this unit
strace -f -tt -xx -s 256 -e trace=read,write -e trace-fds=3 -p <pid> -o /tmp/uart.log
```

Keep that shell session open: strace dies with the telnet session that started it. Then feed from the Tuya app, press buttons,
cover the hopper sensor, and decode the log with [`tools/mcu-decode.py`](../tools/mcu-decode.py).
Writing a frame straight to the port with `echo -ne '\x55\xaa...' > /dev/ttyS0` is enough to test a guess.
