#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <3ds.h>
#include <stdbool.h>

typedef enum {
    PLAYER_STOPPED,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_LOADING,
    PLAYER_ERROR
} PlayerState;

// Initialize the audio player (ndsp, mpg123).
bool audio_player_init(void);

// Clean up audio player resources.
void audio_player_cleanup(void);

// Load and start playing an MP3 from a URL.
bool audio_player_load_url(const char* url);

// Resume playback.
void audio_player_play(void);

// Pause playback.
void audio_player_pause(void);

// Stop playback and unload.
void audio_player_stop(void);

// Toggle play/pause.
void audio_player_toggle(void);

// Must be called every frame to feed audio buffers.
void audio_player_update(void);

// Get current player state.
PlayerState audio_player_get_state(void);

// Get playback progress (0.0 to 1.0).
float audio_player_get_progress(void);

// Get current position in milliseconds.
int audio_player_get_position_ms(void);

// Set the expected duration (from Plex metadata, in ms).
void audio_player_set_duration(int duration_ms);

// Set volume (0.0 to 1.0).
void audio_player_set_volume(float vol);

// Get current volume (0.0 to 1.0).
float audio_player_get_volume(void);

// Get the current download speed in bytes/sec (averaged over recent window).
int audio_player_get_download_speed(void);

// Returns true if buffer underruns indicate the quality should be reduced.
bool audio_player_needs_quality_downgrade(void);

// Clear the quality downgrade flag (call after handling it).
void audio_player_clear_downgrade_flag(void);

// Pre-fetch the next track URL into a secondary buffer for seamless transition.
bool audio_player_prefetch_url(const char* url);

// Check if a pre-fetched track is ready.
bool audio_player_is_prefetch_ready(void);

// Activate the pre-fetched buffer as the current playback stream.
// Returns true if prefetch data was available and activated.
bool audio_player_activate_prefetch(void);

// Cancel any active pre-fetch.
void audio_player_cancel_prefetch(void);

#endif // AUDIO_PLAYER_H
