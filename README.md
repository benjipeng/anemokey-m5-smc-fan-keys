# Anemokey

Anemokey reads Apple SMC fan keys on M5 Macs.

macOS has no public fan-speed API. On Apple silicon, `thermalmonitord` owns fan policy. Fan state lives in the System Management Controller, a key-value device that userspace reaches through IOKit. Anemokey is a small program written from scratch against that interface. It does not wrap another fan-control application, and it does not replace `thermalmonitord`.

## Initial support

M5 only.

The first machine is a MacBook Pro, model identifier Mac17,7, with an M5 Max, running macOS 27. Other generations are out of scope. M3 and M4 expose different mode keys, and this project does not paper over that.

## First milestone

Read the fan keys on this machine.

- how many fans are present
- actual speed
- firmware minimum and maximum
- the mode key, spelled the way this firmware spells it

With no arguments the program only reads. `--rpm` holds every fan at one speed until the process leaves, then writes each mode key back to the value it had before the hold.

## Interface

On this machine the registry entry is `AppleSMCKeysEndpoint`, under `smc@8C600000`. Its user client is `AppleSMCClient`. `thermalmonitord` is already connected. The program calls `IOConnectCallStructMethod` selector 2 with an 80-byte structure. Reads use read-index, read-key-info, and read-bytes. `--rpm` also sends write-bytes, and only as root. Fan-key reads on this Mac succeeded as uid 501. An unprivileged mode write returns `kIOReturnNotPrivileged`.

## Build

```
make
./build/anemokey
sudo ./build/anemokey --rpm 4000
```

`make` writes `build/anemokey`. The read command does not request root. `--rpm` runs in the foreground and applies the speed to every fan reported by `FNum`. The number has to sit inside each fan's firmware minimum and maximum. Ctrl-C sends `SIGINT`. The process then writes the saved mode byte back. `SIGTERM` and `SIGHUP` take the same path. `SIGKILL` does not.

## Observed on this Mac

Read on 2026-09-25 from Mac17,7, macOS 27.0 (26A428), uid 501. The machine has one `AppleSMCKeysEndpoint`. The key index contains 3864 keys. The next index returns SMC status 184 (index out of range).

`FNum` is `ui8 `, value 2.

The mode key spelling is lowercase `F0md` and `F1md`, type `ui8 `, value 0. Uppercase `F0Md` and `F1Md` are absent. `Ftst` is absent. A key-info read of each absent name returns SMC status 132 (key not found).

Actual, minimum, and maximum use type `flt `, little-endian IEEE-754. `F0Mn` bytes `00 d0 10 45` are 2317 rpm. The same bytes read as a big-endian float are not a fan speed. `F0Mx` and `F1Mx` bytes `00 90 f4 45` are 7826 rpm. Minimum and maximum stayed fixed across reads. Actual speed is live: fan 0 moved through 2316–2319 rpm in this session, and fan 1 through 2499–2503 rpm. One fan 0 reading was 1 rpm under the minimum key. The block below is the last run.

Attribute bytes are printed as returned and are not interpreted.

```
model Mac17,7
uid 501
service AppleSMCKeysEndpoint
service_count 1
open ok
keys 3864
index_end smc_result 184 index out of range
count FNum type "ui8 " size 1 attr 0x80 value 2 raw 02
actual F0Ac type "flt " size 4 attr 0x84 value 2318.0 rpm raw 00e01045
minimum F0Mn type "flt " size 4 attr 0x84 value 2317.0 rpm raw 00d01045
maximum F0Mx type "flt " size 4 attr 0x85 value 7826.0 rpm raw 0090f445
mode F0md type "ui8 " size 1 attr 0xd0 value 0 raw 00
actual F1Ac type "flt " size 4 attr 0x84 value 2503.0 rpm raw 00701c45
minimum F1Mn type "flt " size 4 attr 0x84 value 2317.0 rpm raw 00d01045
maximum F1Mx type "flt " size 4 attr 0x85 value 7826.0 rpm raw 0090f445
mode F1md type "ui8 " size 1 attr 0xd0 value 0 raw 00
missing F0Md smc_result 132 key not found
missing F1Md smc_result 132 key not found
missing Ftst smc_result 132 key not found
```

## Write trial

On 2026-09-25 a program outside this repository tried fan 0 only. `F0Mn` was 2317 rpm and `F0Mx` was 7826 rpm. The planned target was 3200 rpm.

As uid 501, writing `1` to `F0md` returned `kIOReturnNotPrivileged` (`0xe00002c1`). The key stayed `0`, and `F0Tg` was not written.

The same program then ran as root, after an administrator prompt. Before the write, `F0md` was 0, `F0Tg` was 2574 rpm, and `F0Ac` was 2577 rpm. Writing `F0md = 1` succeeded, then writing `F0Tg = 3200` succeeded. Over the next samples `F0Tg` stayed 3200 and `F0Ac` moved 2575, 2979, 3177, 3225, 3230, 3217, 3215, 3205. Fan 1 was not written; its actual speed stayed near 2780 rpm.

The process wrote `F0md` back to 0 before exiting. A later unprivileged read showed `F0md` 0, `F0Tg` 2513 rpm, and `F0Ac` 2516 rpm. `F1md` was still 0. No `Ftst` write was sent.
