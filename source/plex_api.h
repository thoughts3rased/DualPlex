#ifndef PLEX_API_H
#define PLEX_API_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

#define PLEX_MAX_ITEMS 500
#define PLEX_PAGE_SIZE 30
#define PLEX_CLIENT_ID "dualplex-3ds"
#define PLEX_PRODUCT "DualPlex"
#define PLEX_VERSION "0.1.0"
#define PLEX_DEVICE "Nintendo 3DS"

typedef struct {
    char key[PLEX_MAX_STR];
    char title[PLEX_MAX_STR];
    char type[64];
    int count;
} PlexLibrary;

typedef struct {
    char rating_key[PLEX_MAX_STR];
    char key[PLEX_MAX_URL];
    char title[PLEX_MAX_STR];
    char summary[512];
    char thumb[PLEX_MAX_URL];
} PlexArtist;

typedef struct {
    char rating_key[PLEX_MAX_STR];
    char key[PLEX_MAX_URL];
    char title[PLEX_MAX_STR];
    int leaf_count;
    int duration;
} PlexPlaylist;

typedef struct {
    char rating_key[PLEX_MAX_STR];
    char key[PLEX_MAX_URL];
    char title[PLEX_MAX_STR];
    char parent_title[PLEX_MAX_STR];
    char thumb[PLEX_MAX_URL];
    int year;
    int leaf_count;
} PlexAlbum;

typedef struct {
    char rating_key[PLEX_MAX_STR];
    char title[PLEX_MAX_STR];
    char grandparent_title[PLEX_MAX_STR];
    char parent_title[PLEX_MAX_STR];
    char thumb[PLEX_MAX_URL];
    char part_key[PLEX_MAX_URL];
    char audio_codec[32];
    int bitrate;
    int sampling_rate;
    int bit_depth;
    int duration;
    int index;
    float user_rating; // Plex's 0.0-10.0 star rating (0 = unrated); each star = 2.0
    // Rating keys of this track's parent album/artist ("parentRatingKey" /
    // "grandparentRatingKey" in Plex's JSON) - not used for anything in the
    // normal online browsing flow (grandparent_title/parent_title above
    // already cover display), but offline.c needs stable ids (not just
    // display titles, which can collide) to group downloaded tracks back
    // into artists/albums and to know which tracks a given album/artist
    // download should include. Populated for every track-list endpoint
    // (they all funnel through parse_tracks_from_json()), so it's available
    // regardless of which screen a download was queued from.
    char album_rating_key[PLEX_MAX_STR];
    char artist_rating_key[PLEX_MAX_STR];
} PlexTrack;

typedef struct {
    int id;
    char code[16];
    char auth_token[128];
    bool expired;
} PlexPin;

typedef struct {
    char name[PLEX_MAX_STR];
    char uri[PLEX_MAX_URL];
    char access_token[128];
    bool is_local;
} PlexServerResource;

// Audio quality tiers for adaptive streaming
typedef enum {
    QUALITY_FLAC_DIRECT,  // N3DS only: direct FLAC (highest quality, ~1000+ kbps)
    QUALITY_MP3_320,      // Transcode to 320 kbps MP3
    QUALITY_MP3_192,      // Transcode to 192 kbps MP3  
    QUALITY_MP3_128,      // Transcode to 128 kbps MP3
    QUALITY_MP3_64,       // Transcode to 64 kbps MP3 (lowest quality fallback)
    QUALITY_TIER_COUNT
} PlexQualityTier;

// Get/set the current adaptive quality tier.
PlexQualityTier plex_api_get_quality_tier(void);
void plex_api_set_quality_tier(PlexQualityTier tier);

// Step down to next lower quality tier. Returns false if already at lowest.
bool plex_api_quality_step_down(void);

// Get a human-readable label for the current quality tier.
const char* plex_api_get_quality_label(PlexQualityTier tier);

// Suggest the best quality tier for a given measured download speed (bytes/sec).
PlexQualityTier plex_api_suggest_quality(int download_speed_bps, bool is_n3ds);

// Sets the per-console X-Plex-Client-Identifier sent with every request
// (overriding the default shared PLEX_CLIENT_ID literal). Call once at
// startup with a value generated once per console and persisted to
// config.txt (see config_ensure_client_id() and main.c), before any other
// plex_api_* call - including the login-flow ones like plex_api_create_pin()
// and plex_api_login_direct(), which happen before plex_api_init() since
// there's no server yet at that point. Without a stable per-console id, PMS
// can't tell two 3DS's signed into the same account apart on its Now
// Playing dashboard - it keys "players" by this value alone, so both
// consoles look like a single player, each one's timeline update stomping
// the other's. A NULL or empty client_id is ignored.
void plex_api_set_client_id(const char* client_id);

// Initialize the Plex API client with server URL and auth token.
bool plex_api_init(const char* server_url, const char* auth_token);

// Clean up resources.
void plex_api_cleanup(void);

// Get current token and server URL.
const char* plex_api_get_token(void);
const char* plex_api_get_server_url(void);

// The per-console id set by plex_api_set_client_id() (or the default shared
// PLEX_CLIENT_ID if that was never called) - for building an
// "X-Plex-Client-Identifier" header outside plex_api.c itself (see
// offline.c's direct-download requests, which build their own header list
// rather than going through a plex_api_* helper).
const char* plex_api_get_client_id(void);

// Whether the current connection is HTTPS rather than plain HTTP.
bool plex_api_is_https(void);

// Whether the current server address is on a private/local network
// (RFC1918, loopback, or link-local) rather than a public/remote one.
bool plex_api_is_local_connection(void);

// Test if the server is reachable and the token is valid.
bool plex_api_test_connection(void);

// Whether the most recent plex_api_test_connection() call succeeded -
// distinct from just having a server_url/token configured at all. Drives
// whether the top bar shows the local/remote + HTTP/HTTPS icons or a
// single "disconnected" one instead (see ui_render_top()).
bool plex_api_is_connected(void);

// Create a PIN for plex.tv/link authorization flow.
bool plex_api_create_pin(PlexPin* out_pin);

// Check status of a PIN. Sets out_pin->auth_token if authorized.
bool plex_api_check_pin(PlexPin* pin);

// Direct sign-in with username/password and optional 2FA code.
int plex_api_login_direct(const char* login, const char* password, const char* code_2fa, char* out_token, size_t max_token);

// Get list of Plex Media Servers associated with account token.
int plex_api_get_servers(const char* account_token, PlexServerResource* out_servers, int max);

// Re-establishes a connection using saved account credentials when the
// previously working server address can no longer be reached at all -
// e.g. launching on a different network (a mobile hotspot, a different
// WiFi) where that particular address isn't reachable, but a different
// address for the same server (its remote/relay address, say) still is.
// `server_name` should be AppConfig.server_name (blank is fine - falls
// back to the account's first server). On success, plex_api_init() has
// already been called with whatever candidate worked (readable via
// plex_api_get_server_url()/plex_api_get_token()); the caller should
// persist those via config_save().
bool plex_api_reconnect_via_account(const char* account_token, const char* server_name);

// Get music libraries.
int plex_api_get_music_libraries(PlexLibrary* out, int max);

// Get artists in a library with pagination.
int plex_api_get_artists_page(const char* library_key, PlexArtist* out, int start, int count, int* out_total);
int plex_api_get_artists(const char* library_key, PlexArtist* out, int max);

// Get albums for an artist with pagination.
int plex_api_get_albums_page(const char* artist_key, PlexAlbum* out, int start, int count, int* out_total);
int plex_api_get_albums(const char* artist_key, PlexAlbum* out, int max);

// Get tracks for an album/playlist with pagination.
int plex_api_get_tracks_page(const char* album_key, PlexTrack* out, int start, int count, int* out_total);
int plex_api_get_tracks(const char* album_key, PlexTrack* out, int max);

// Get playlists in account/server.
int plex_api_get_playlists(PlexPlaylist* out, int max);

// Get tracks in a playlist.
int plex_api_get_playlist_tracks(const char* playlist_key, PlexTrack* out, int max);

// Search tracks by query string.
int plex_api_search_tracks(const char* query, PlexTrack* out, int max);

// Get recently added music tracks.
int plex_api_get_recently_added(PlexTrack* out, int max);

// Build a direct stream URL for a track into url_out.
bool plex_api_get_stream_url(const PlexTrack* track, char* url_out, size_t url_max);

// Build a transcode-to-MP3 URL for a track into url_out.
bool plex_api_get_transcode_url(const PlexTrack* track, char* url_out, size_t url_max);

// Build a transcode-to-MP3 URL that starts at seek_ms into the track, for
// implementing seeking (the player reloads from this URL and calls
// audio_player_set_position_offset_ms(seek_ms) to match).
bool plex_api_get_seek_url(const PlexTrack* track, int seek_ms, char* url_out, size_t url_max);

// Build a URL for saving a track to the SD card for offline playback
// (offline.c). Unlike plex_api_get_stream_url(), this never transcodes -
// it always points at the track's original file (server_url + part_key),
// regardless of the current adaptive quality tier or console model, so a
// download captures the source file as-is and a download in progress never
// contends with a currently-playing track for one of the server's limited
// concurrent transcode sessions.
bool plex_api_get_download_url(const PlexTrack* track, char* url_out, size_t url_max);

// Same idea as plex_api_get_download_url(), for a track that isn't already
// in a format the 3DS can decode (offline.c's downloader falls back to this
// when the source needs transcoding) - unlike that one, this DOES go through
// the same universal transcoder as live playback, since forcing MP3 output
// has no equivalent on the plain file-download path. Uses its own transcode
// session id (distinct from plex_api_get_transcode_url()'s, even for the
// exact same track) so downloading a track doesn't collide with - and break
// - a live-playback transcode session for that same track.
bool plex_api_get_download_transcode_url(const PlexTrack* track, char* url_out, size_t url_max);

typedef struct {
    int time_ms;
    char text[128];
} PlexLyricLine;

// Download album cover art image bytes
bool plex_api_get_album_art(const char* thumb_path, u8** out_data, size_t* out_size);

// Fetch time-synced lyrics for a track. Blocks the caller for the duration
// of two HTTP round-trips - do not call this while a track is actively
// loading/playing (freezes rendering and input for however long that takes).
// Use the async version below in that context instead.
int plex_api_get_lyrics(const char* rating_key, PlexLyricLine* out, int max);

// Report session timeline to Plex Media Server. Blocks the caller for one
// HTTP round-trip - same caution as plex_api_get_lyrics() above.
void plex_api_report_timeline(const char* rating_key, const char* state, int time_ms, int duration_ms);

// --- Non-blocking equivalents ------------------------------------------
// Both need their *_async_update() pumped once per frame (same pattern as
// album_art_update()) regardless of whether a fetch is currently in flight.

// Starts a non-blocking lyrics fetch for rating_key, replacing any fetch
// already in progress. Call plex_api_lyrics_async_update() every frame to
// pump it, then plex_api_lyrics_async_is_done() to check readiness and
// plex_api_lyrics_async_take_result() once to collect it (that call resets
// state back to idle - only take the result once per start()).
void plex_api_lyrics_async_start(const char* rating_key);
void plex_api_lyrics_async_update(void);
bool plex_api_lyrics_async_is_done(void);
int plex_api_lyrics_async_take_result(PlexLyricLine* out, int max);

// Fire-and-forget non-blocking timeline report. A newer call replaces an
// older one still in flight rather than queuing (these are periodic
// best-effort pings - dropping a stale one is fine).
void plex_api_report_timeline_async(const char* rating_key, const char* state, int time_ms, int duration_ms);
void plex_api_timeline_async_update(void);

// Fire-and-forget non-blocking star rating. rating_10 is Plex's 0.0-10.0
// scale (2 points per star; rounded to the nearest whole point), or -1 to
// clear the rating back to unrated. Shares the same in-flight request slot
// as plex_api_report_timeline_async() above (a newer call of either
// replaces whatever's still in flight) - rating happens rarely enough that
// occasionally dropping a periodic timeline ping in favor of it is a fine
// tradeoff, same "best-effort" reasoning that already applies to timeline
// reports colliding with each other.
void plex_api_rate_track_async(const char* rating_key, float rating_10);

// --- Sonic Analysis (loudness) -------------------------------------------
// Plex's server-side per-track loudness analysis (Plex Pass "Sonic
// Analysis"), used to drive smart crossfades: a track-level normalization
// gain plus a short window of Plex's own short-term-loudness curve (dB,
// one sample per 100ms) near whichever end of the track is relevant. Not
// every track has this - unanalyzed tracks (or a server without Plex Pass)
// come back with .valid = false and callers should fall back to a plain
// fixed-duration crossfade with no gain adjustment.
#define PLEX_ANALYSIS_CURVE_SAMPLES 100 // 10s of curve at 100ms/sample

typedef struct {
    bool valid;                                   // false if this track has no Sonic Analysis data
    float gain_db;                                 // Stream.gain - normalization offset to apply
    float loudness_lufs;                            // Stream.loudness - integrated loudness
    float lra;                                      // Stream.lra - loudness range
    int curve_count;                                 // valid entries in curve_db
    float curve_db[PLEX_ANALYSIS_CURVE_SAMPLES];      // short-term loudness, oldest -> newest
} PlexTrackAnalysis;

// Starts a non-blocking Sonic Analysis fetch for rating_key, replacing any
// fetch already in progress. want_tail selects which end of the track's
// loudness curve to return: true for the last ~10s (use when this track is
// about to end and you're timing a fade-out), false for the first ~10s (use
// for the track about to start, timing a fade-in). Pump with
// plex_api_analysis_async_update() every frame, same pattern as lyrics.
void plex_api_analysis_async_start(const char* rating_key, bool want_tail);
void plex_api_analysis_async_update(void);
bool plex_api_analysis_async_is_done(void);
// Collects the result (resets state back to idle - only call once per
// start()). Always succeeds once is_done() is true; check out->valid for
// whether real analysis data was actually found.
void plex_api_analysis_async_take_result(PlexTrackAnalysis* out);

#endif // PLEX_API_H
