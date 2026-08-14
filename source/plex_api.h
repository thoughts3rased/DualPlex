#ifndef PLEX_API_H
#define PLEX_API_H

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"

#define PLEX_MAX_ITEMS 500
#define PLEX_PAGE_SIZE 30
#define PLEX_CLIENT_ID "3ds-plex-client"
#define PLEX_PRODUCT "3DS Plex Client"
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

// Initialize the Plex API client with server URL and auth token.
bool plex_api_init(const char* server_url, const char* auth_token);

// Clean up resources.
void plex_api_cleanup(void);

// Get current token and server URL.
const char* plex_api_get_token(void);
const char* plex_api_get_server_url(void);

// Test if the server is reachable and the token is valid.
bool plex_api_test_connection(void);

// Create a PIN for plex.tv/link authorization flow.
bool plex_api_create_pin(PlexPin* out_pin);

// Check status of a PIN. Sets out_pin->auth_token if authorized.
bool plex_api_check_pin(PlexPin* pin);

// Direct sign-in with username/password and optional 2FA code.
int plex_api_login_direct(const char* login, const char* password, const char* code_2fa, char* out_token, size_t max_token);

// Get list of Plex Media Servers associated with account token.
int plex_api_get_servers(const char* account_token, PlexServerResource* out_servers, int max);

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

typedef struct {
    int time_ms;
    char text[128];
} PlexLyricLine;

// Download album cover art image bytes
bool plex_api_get_album_art(const char* thumb_path, u8** out_data, size_t* out_size);

// Fetch time-synced lyrics for a track
int plex_api_get_lyrics(const char* rating_key, PlexLyricLine* out, int max);

// Report session timeline to Plex Media Server
void plex_api_report_timeline(const char* rating_key, const char* state, int time_ms, int duration_ms);

#endif // PLEX_API_H
