#ifndef OFFLINE_H
#define OFFLINE_H

#include <stdbool.h>
#include <stddef.h>
#include "plex_api.h"

// Everything offline.c stores on the SD card lives under here: downloaded
// audio in tracks/, one cached thumbnail per album in thumbs/, one cached
// time-synced lyrics file per track (best-effort - see maybe_cache_lyrics())
// in lyrics/, a JSON manifest recording what's been downloaded (and each
// track's reference count - see offline_queue_tracks()) so it survives a
// relaunch, and (not exposed here - purely an implementation detail) a small
// resume record for whichever download was in progress, so an app close or
// network drop mid-download picks back up instead of starting over.
#define OFFLINE_DIR          "/3ds/dualplex/offline"
#define OFFLINE_TRACKS_DIR   "/3ds/dualplex/offline/tracks"
#define OFFLINE_THUMBS_DIR   "/3ds/dualplex/offline/thumbs"
#define OFFLINE_LYRICS_DIR   "/3ds/dualplex/offline/lyrics"
#define OFFLINE_MANIFEST_PATH "/3ds/dualplex/offline/library.json"

// Must be called once at startup (after logger_init(), before any other
// offline_* call, on the main thread) - loads the on-disk manifest, picks up
// an interrupted download left over from a previous run if there is one
// (see the module comment at the top of offline.c), and spawns the
// background thread that actually pumps downloads - pinned to core 2 on
// New3DS (falls back to sharing a core with everything else if that's not
// available, same as Old3DS), so a download in progress no longer costs the
// main thread/render loop anything.
void offline_init(void);

// Publishes whether audio_player.c is currently streaming a track over the
// network, for the background download thread to read (see
// audio_player_is_streaming_over_network()) - call every frame on the main
// thread, right after audio_player_update(). Kept as a plain published flag
// rather than having the download thread call into audio_player.c directly,
// since audio_player.c's own state is main-thread-only otherwise.
void offline_set_network_streaming_hint(bool is_streaming);

// Publishes the quality tier (AppConfig.download_quality, Settings > Download
// Quality) new downloads should be fetched at - call once at startup right
// after offline_init() with the loaded config value, and again any time the
// user changes the setting. Same plain-published-flag approach as
// offline_set_network_streaming_hint() above, for the same reason (read by
// the background download thread, written from the main thread). Doesn't
// affect a download already in flight - only ones started after the call.
void offline_set_download_quality(PlexQualityTier tier);

// Stops the background download thread and waits for it to actually exit -
// call once at shutdown, on the main thread, before curl_global_cleanup().
// A download that was still in progress is left exactly where it was (not
// cancelled) if it's resumable, so it picks back up on the next launch
// instead of starting over - see offline_init().
void offline_cleanup(void);

// --- Queueing downloads -----------------------------------------------------
// Queues `tracks` for background download. A track already fully downloaded
// just has its reference count bumped rather than being re-downloaded (see
// the delete functions below for what the count is for); a track already
// waiting in the queue from an earlier call is left alone rather than
// duplicated. Returns how many of `tracks` were NOT already downloaded
// before this call (i.e. newly queued) - callers use this to report e.g.
// "Queued 12 tracks" / "Already downloaded" to the user.
int offline_queue_tracks(const PlexTrack* tracks, int count);

// Same as offline_queue_tracks(), and additionally records an offline
// playlist named `playlist_title` (keyed by `playlist_rating_key`)
// referencing exactly these tracks in this order, so it shows up via
// offline_get_playlists()/offline_get_tracks_for_playlist(). Re-downloading
// an already-offline playlist replaces its track list with this one rather
// than duplicating the playlist entry.
int offline_queue_playlist(const char* playlist_rating_key, const char* playlist_title,
                            const PlexTrack* tracks, int count);

// --- Download engine status --------------------------------------------------
typedef struct {
    bool active;             // a transfer is currently in flight
    char title[PLEX_MAX_STR];
    char artist_title[PLEX_MAX_STR];
    size_t bytes_done;
    size_t bytes_total;      // 0 if the server hasn't reported Content-Length yet
    int queue_remaining;     // items still waiting behind the current one (0 if !active)
} OfflineDownloadStatus;
void offline_get_download_status(OfflineDownloadStatus* out);

// --- Querying what's on disk -------------------------------------------------
bool offline_track_is_downloaded(const char* rating_key);

// The bitrate (kbps) this track's downloaded copy was actually saved at (see
// the download engine / Settings > Download Quality) - -1 if it isn't
// downloaded. For a directly-saved lossless source (Download Quality:
// Original, a FLAC file) this is whatever real bitrate Plex reports for it,
// not a tier cap - typically well above anything plex_api_estimate_stream_
// bitrate() could return, so it compares favorably without needing special-
// casing. Used by ui.c's "Prefer Downloaded Media: On poor connections"
// setting to compare against that live streaming estimate for the same
// track.
int offline_get_track_bitrate(const char* rating_key);

// Whether ANY track under this artist/playlist has been downloaded - used
// to grey out an otherwise-unavailable-offline entry in a cached library
// listing (see library_cache.h) rather than requiring it to be *fully*
// downloaded before it's shown as accessible at all.
bool offline_artist_has_any_downloaded(const char* artist_rating_key);
bool offline_playlist_has_any_downloaded(const char* playlist_rating_key);

// Builds the "file://<sdmc path>" URL audio_player_load_url()/
// audio_player_start_crossfade() accept for offline playback (see
// audio_player.c's Deck.is_local handling). Returns false if the track
// isn't downloaded.
bool offline_get_track_playback_url(const char* rating_key, char* url_out, size_t url_max);

// Path to this track's album's cached thumbnail (one file shared by every
// track in the same album - see the download engine's thumbnail handling),
// for album_art_load_local(). Returns false if none was cached.
bool offline_get_thumb_path(const char* rating_key, char* path_out, size_t path_max);

// This track's cached time-synced lyrics (see maybe_cache_lyrics(), fetched
// best-effort alongside the track itself when it was downloaded) - same
// signature/convention as plex_api_get_lyrics(): always returns at least 1
// line, a placeholder "No time-synced lyrics available" one if nothing was
// cached (never downloaded, or the server genuinely had none for this track
// at download time), so a caller never needs to special-case "0 lines" for
// offline playback the way it would for a still-in-flight online fetch. 0
// only for a bad rating_key/out/max.
int offline_get_lyrics(const char* rating_key, PlexLyricLine* out, int max);

size_t offline_get_storage_used_bytes(void);
int offline_get_track_count(void);
int offline_get_album_count(void);
int offline_get_artist_count(void);
int offline_get_playlist_count(void);

// --- Browsing downloaded content (mirrors plex_api.h's online equivalents,
// same PlexArtist/PlexAlbum/PlexTrack/PlexPlaylist shapes so ui.c's existing
// list screens can render either source unmodified) -------------------------
//
// PlexArtist.key/PlexAlbum.key/PlexPlaylist.key are set to the bare rating
// key (not a "/children"-style URL like the online versions) - offline
// browsing has no server endpoint to page through, everything's already
// local, so these just get passed straight back into the *_for_*() calls
// below instead of into plex_api_get_*_page().
int offline_get_artists(PlexArtist* out, int max);
int offline_get_albums(const char* artist_rating_key, PlexAlbum* out, int max);
int offline_get_tracks_for_album(const char* album_rating_key, PlexTrack* out, int max);
int offline_get_playlists(PlexPlaylist* out, int max);
int offline_get_tracks_for_playlist(const char* playlist_rating_key, PlexTrack* out, int max);

// --- Deleting downloads -------------------------------------------------------
// Each decrements the reference count of every track it covers, actually
// removing a track's file (and, if no other downloaded track in the same
// album still needs it, the shared album thumbnail) only once nothing
// references it anymore - see offline_queue_tracks()'s comment.
void offline_delete_track(const char* rating_key);
void offline_delete_album(const char* album_rating_key);
void offline_delete_artist(const char* artist_rating_key);
// Also removes the playlist's own manifest entry unconditionally (nothing
// else references a playlist entry the way tracks reference albums/artists).
void offline_delete_playlist(const char* playlist_rating_key);
// Deletes every downloaded track, cached thumbnail, and playlist, and clears
// any in-progress/queued downloads. Used by the "Delete All Downloads" row
// on SCREEN_DOWNLOADS.
void offline_delete_all(void);

#endif // OFFLINE_H
