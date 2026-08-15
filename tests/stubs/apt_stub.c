/*
 * Test-controllable stand-in for APT_CheckNew3DS(). Real hardware detection
 * doesn't exist on the host, so tests set g_stub_is_new3ds directly before
 * calling into plex_api functions that branch on New3DS detection (see
 * plex_api_get_stream_url()'s FLAC-direct-stream path).
 */
#include "3ds.h"

bool g_stub_is_new3ds = false;

int APT_CheckNew3DS(bool* out) {
    if (out) *out = g_stub_is_new3ds;
    return 0;
}
