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

// Offsets the value audio_player_get_position_ms() reports, without touching
// decoded-sample tracking. Used after a seek: the reloaded stream always
// starts decoding from sample 0, so this is how the UI's displayed position
// (and progress bar) keeps reading the real playback position instead of
// resetting to 0. audio_player_load_url() resets this back to 0, so call it
// again afterward for a seek.
void audio_player_set_position_offset_ms(int offset_ms);

// Copies up to max_samples of the most recently decoded audio into out, as
// mono s16 samples (stereo downmixed by averaging L+R) - for driving a
// visualizer. Returns how many samples were actually copied (0 if nothing
// has been decoded yet). Not a ring/history buffer - just whatever's freshest
// as of the last decode, refreshed every ~4096-sample chunk.
int audio_player_get_visualizer_samples(s16* out, int max_samples);

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

// True once the current track's stream has fully downloaded (or errored out).
// Some Plex servers only permit one concurrent transcode session; starting a
// prefetch for the next track while the current one is still an in-flight
// transcode races both requests for that single slot, and can 400 either one
// (including the track that's actively playing). Callers should wait for
// this before prefetching to avoid that.
bool audio_player_is_download_finished(void);

// Activate the pre-fetched buffer as the current playback stream.
// Returns true if prefetch data was available and activated.
bool audio_player_activate_prefetch(void);

// Cancel any active pre-fetch.
void audio_player_cancel_prefetch(void);

#endif // AUDIO_PLAYER_H
