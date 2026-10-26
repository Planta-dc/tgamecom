# Vendored miniaudio

- Project: [miniaudio](https://github.com/mackron/miniaudio)
- Upstream source: `https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h`
- Vendored version: **0.11.25** (version/date are recorded in the header; retrieved 2026-10-26)
- SHA-256: `ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89`
- License: the unmodified header includes the author's Unlicense and MIT-0 alternatives at its end.

The optional SDL frontend compiles this header as its host playback backend. No separate audio SDK or MAME runtime is required. The emulator's sound synthesis is implemented separately in `src/standalone/audio_synth.cpp`.
