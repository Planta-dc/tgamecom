# Local Game.com system firmware

This directory contains the external Game.com system firmware supplied for development. The raw files are extracted so the standalone emulator can read them directly:

- `internal.bin` — 4096 bytes
- `external.bin` — 262144 bytes

The original ZIP is retained locally as `gamecom.zip`. Their CRC32 values are `a0cec361` and `e235a589`, respectively. These hashes were cross-checked against the MAME reference, but using the files does not make MAME a runtime dependency.

Firmware and retail game ROMs are intentionally excluded from source archives and version control. Obtain and use them only where authorized.
