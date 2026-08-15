/*
 * No-op stand-in for audio_player_update(). plex_api.c's
 * perform_blocking_request() calls the real one to keep NDSP fed while a
 * blocking Plex API request is in flight (see its comment in plex_api.c) -
 * on the host there's no NDSP/audio pipeline to feed, so this just needs to
 * exist for the linker.
 */
#include "audio_player.h"

void audio_player_update(void) {}
