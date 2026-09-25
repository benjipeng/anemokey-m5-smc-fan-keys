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

No fan speed is written. A mode write takes the fan away from the system. That waits until a read shows which keys exist here.

## Interface

On this machine the registry entry is `AppleSMCKeysEndpoint`, under `smc@8C600000`. Its user client is `AppleSMCClient`. `thermalmonitord` is already connected. The probe calls `IOConnectCallStructMethod` selector 2 with an 80-byte structure. It sends read-index, read-key-info, and read-bytes only. Fan-key reads on this Mac succeeded as uid 501. Writes are not part of this milestone.

## Build

```
make
./build/anemokey
```

`make` writes `build/anemokey`. The probe does not request root.

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
