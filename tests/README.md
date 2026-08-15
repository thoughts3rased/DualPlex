# Unit tests

Host-run unit tests for the transcode/stream-URL logic in `source/plex_api.c`
that was the source of the "400 error on transcode" bugs - the actual
production file compiled with a native compiler, not a reimplementation of
its logic.

## Why these exist

The main app targets the 3DS via devkitARM's cross-compiler, which only
produces `.3dsx`/`.elf` binaries that need real 3DS hardware (or an emulator)
to run - there's no way to execute the shipped code directly in a normal dev
environment. `plex_api.c`'s URL-building and stream/transcode decision logic
is almost entirely plain C string handling with no 3DS-specific dependencies
though (just one call to `APT_CheckNew3DS` for New3DS detection), so it can
be compiled and run with a regular host compiler once that one call is
stubbed out. See `stubs/3ds.h` for exactly what's stubbed and why.

## Running

```
make -f tests/Makefile
```

Requires a native `gcc` and `libcurl` dev headers/lib on your machine (the
real libcurl is linked in - not a fake one - so `path=` percent-encoding is
exercised exactly as it runs on-device). On Windows via MSYS2/devkitPro's
bundled shell:

```
pacman -S --needed gcc libcurl-devel
make -f tests/Makefile
```

`make -f tests/Makefile clean` removes `tests/build/`.

## What's covered

- `plex_api_get_transcode_url()`:
  - `QUALITY_FLAC_DIRECT` never produces `audioBitrate=0` (the original bug -
    that tier means "skip transcoding", but the function can still be reached
    with it active, and Plex 400s on a zero bitrate)
  - transcode bitrate matches the selected quality tier
  - the URL never duplicates `X-Plex-Token`/`X-Plex-Client-Identifier`/
    `X-Plex-Product` as query params (those belong on headers only - Plex
    400s if identity is asserted both ways)
  - each request gets its own `session=` id derived from the track's rating
    key, not a single hardcoded value (concurrent current-track + prefetch
    transcodes were colliding on one shared session id and 400ing both)
  - `path=` is built correctly from a bare rating key, an already-prefixed
    `/library/metadata/...` key (no double-prefixing), and falls back to
    `part_key` when there's no rating key
- `plex_api_get_stream_url()`:
  - direct-streams an MP3 that fits inside the current quality tier's bitrate
  - falls through to transcode when the source exceeds the tier's bitrate
  - direct-streams FLAC on New3DS when the `FLAC_DIRECT` tier is selected
  - on non-New3DS hardware, `FLAC_DIRECT` falls back to a transcode with a
    real bitrate (not 0) rather than direct-streaming

## What's *not* covered here

The `X-Plex-Platform: Generic` / no-`X-Plex-Client-Identifier`-header fix in
`audio_player.c` isn't exercised by this suite - that file pulls in
citro2d/citro3d/ndsp/mpg123, which only exist inside the devkitARM sysroot,
so it can't be compiled for the host without a much larger stubbing effort.
That fix is a couple of static header strings; if it regresses, `grep -n
"X-Plex-Platform\|X-Plex-Client-Identifier" source/audio_player.c` is enough
to spot it by eye.
