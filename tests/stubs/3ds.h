#ifndef TESTS_STUB_3DS_H
#define TESTS_STUB_3DS_H
/*
 * Minimal stand-in for devkitPro's <3ds.h>, used only to host-compile and
 * unit test the platform-independent parts of plex_api.c (and, transitively,
 * audio_player.h, which plex_api.c now includes for audio_player_update() -
 * see perform_blocking_request() in plex_api.c) on a regular PC.
 *
 * plex_api.c/.h use exactly two things from the real header: the `u8` type
 * and APT_CheckNew3DS(). That's it - checked with:
 *   grep -noE '\b(APT_\w+|svc\w+|hid\w+|gfx\w+|C3D_\w+|...)\b' source/plex_api.c
 * audio_player.h additionally needs `s16` for its visualizer-samples getter.
 * If plex_api.c/audio_player.h start using more of the real API, add it here.
 */
#include <stdbool.h>

typedef unsigned char u8;
typedef short s16;

/* Real signature returns a Result (a bitfield error code); plex_api.c only
 * reads the bool out-param and ignores the return value, so the stub result
 * type doesn't matter here. */
int APT_CheckNew3DS(bool* out);

#endif /* TESTS_STUB_3DS_H */
