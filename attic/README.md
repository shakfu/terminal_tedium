# attic

Superseded code, kept only for reference. Nothing here is built or supported.

- `adc2FUDI.c` — the TCP/FUDI ADC bridge that predated the OSC client. It was
  already dead code (referenced only from commented-out lines in `software/pdpd`)
  and contained a two-byte heap overflow: it allocated `msgLength` bytes and then
  wrote up to `msgLength` bytes starting at offset 2, with the following
  `strlen()` depending on a NUL that could land outside the allocation. The
  buffer was also leaked on every message inside an infinite loop. It was moved
  here rather than fixed, because `software/OSC client` replaces it.

- `install.sh.2019` — the original installer. Retired rather than fixed: on any
  current Raspberry Pi OS it writes `/boot/config.txt`, which since Bookworm is
  a placeholder whose contents tell you not to edit it, so SPI and I2S were
  never enabled and the codec never appeared. It also hardcoded `/home/pi` and
  the `pi` user (not default since Bullseye), fetched Pd over plain HTTP with no
  checksum, linked the deprecated wiringPi, deleted its own sources on the way
  out, and rebooted unconditionally. Replaced by `install.sh` at the repo root.
