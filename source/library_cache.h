#ifndef LIBRARY_CACHE_H
#define LIBRARY_CACHE_H

#include "plex_api.h"

// A lightweight snapshot of the last successfully-fetched Playlists and
// Artists lists, persisted to the SD card - not the tracks/audio themselves
// (see offline.h for that), just enough of the *shape* of the library to
// still show it, with only the downloaded portion actually playable, the
// next time there's no connection at all. Matches how other music apps
// handle offline mode: the library doesn't just vanish when you lose
// signal, only what wasn't saved for offline stops being playable.
#define LIBRARY_CACHE_PATH "/3ds/dualplex/offline/browse_cache.json"

// Must be called once at startup (after logger_init()), before any other
// library_cache_* call. Loads the snapshot from SD into memory.
void library_cache_init(void);

// Snapshot the given list to SD, replacing whatever was cached before for
// the same kind of list - call this right after every successful online
// fetch of it, so it's there as a fallback next time that fetch fails.
void library_cache_save_playlists(const PlexPlaylist* list, int count);
// `library_key` identifies which library section this artist list belongs
// to (e.g. what was passed to plex_api_get_artists()) - only one library's
// worth of artists is cached at a time (the last one successfully fetched).
void library_cache_save_artists(const char* library_key, const PlexArtist* list, int count);

// Loads a cached snapshot back out, for when the live fetch it would
// normally come from just failed. Returns how many entries were loaded (0
// if there's nothing cached yet).
int library_cache_load_playlists(PlexPlaylist* out, int max);
// Returns 0 without touching `out` if the cached artists snapshot belongs to
// a different library_key than requested (or there isn't one yet).
int library_cache_load_artists(const char* library_key, PlexArtist* out, int max);

#endif // LIBRARY_CACHE_H
