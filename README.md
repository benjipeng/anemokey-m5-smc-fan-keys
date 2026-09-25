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

On this machine the registry entry is `AppleSMCKeysEndpoint`, under `smc@8C600000`. Its user client is `AppleSMCClient`. `thermalmonitord` is already connected. Reads of many keys do not require root. Writes to fan mode and target speed do, and they are not part of the first milestone.

## Status

The program is not written yet. This repository starts with the scope above. Key names are not claimed until they are read on this M5.
