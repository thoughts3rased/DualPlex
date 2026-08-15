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

// Initialize the audio player (ndsp, mpg123). Sets up two independent
// decode/NDSP-channel "decks" - see the crossfading section below for why.
bool audio_player_init(void);

// Clean up audio player resources.
void audio_player_cleanup(void);

// Load and start playing a track from a URL on the active deck, hard-cutting
// whatever was playing before (and cancelling any in-progress crossfade
// first). Use this for manual track changes - skip/seek/quality-change -
// where an instant response matters more than a smooth transition. For
// advancing to the next queued track normally, use
// audio_player_start_crossfade() instead so it blends rather than cuts.
bool audio_player_load_url(const char* url);

// Resume playback.
void audio_player_play(void);

// Pause playback.
void audio_player_pause(void);

// Stop playback and unload.
void audio_player_stop(void);

// Toggle play/pause.
void audio_player_toggle(void);

// Must be called every frame to feed audio buffers - both decks' worth,
// while a crossfade is in progress.
void audio_player_update(void);

// Get current player state (reflects the active deck - the track the user
// currently perceives as "now playing").
PlayerState audio_player_get_state(void);

// Get playback progress (0.0 to 1.0), of the active deck.
float audio_player_get_progress(void);

// Get current position in milliseconds, of the active deck.
int audio_player_get_position_ms(void);

// Set the expected duration (from Plex metadata, in ms) of the active deck.
void audio_player_set_duration(int duration_ms);

// Offsets the value audio_player_get_position_ms() reports for the active
// deck, without touching decoded-sample tracking. Used after a seek: the
// reloaded stream always starts decoding from sample 0, so this is how the
// UI's displayed position (and progress bar) keeps reading the real playback
// position instead of resetting to 0. audio_player_load_url() resets this
// back to 0, so call it again afterward for a seek.
void audio_player_set_position_offset_ms(int offset_ms);

// Copies up to max_samples of the most recently decoded audio into out, as
// mono s16 samples (stereo downmixed by averaging L+R) - for driving a
// visualizer. Always reflects the active deck, even mid-crossfade. Returns
// how many samples were actually copied (0 if nothing has been decoded yet).
// Not a ring/history buffer - just whatever's freshest as of the last
// decode, refreshed every ~4096-sample chunk.
int audio_player_get_visualizer_samples(s16* out, int max_samples);

// Get the current download speed in bytes/sec (averaged over recent window,
// active deck only).
int audio_player_get_download_speed(void);

// Returns true if buffer underruns indicate the quality should be reduced.
bool audio_player_needs_quality_downgrade(void);

// Clear the quality downgrade flag (call after handling it).
void audio_player_clear_downgrade_flag(void);

// True once the active deck's stream has fully downloaded (or errored out).
// Some Plex servers only permit one concurrent transcode session - starting
// a crossfade (or a manual load) while the active deck's own stream is still
// an in-flight transcode races both requests for that single slot, and can
// 400 either one (including the track that's actively playing). Callers
// should wait for this before starting a crossfade to avoid that.
bool audio_player_active_download_finished(void);

// Sets the active deck's loudness-normalization gain (dB, from Plex's Sonic
// Analysis - see plex_api's PlexTrackAnalysis). Ramps in over a few hundred
// ms rather than snapping, since this typically arrives a moment after the
// track's already started playing (it's a separate async fetch). Pass 0 for
// "no adjustment" (unanalyzed track, or analysis unavailable).
void audio_player_set_track_gain(float gain_db);

// --- Crossfading ----------------------------------------------------------
// Two independent decode pipelines ("decks"), each with its own NDSP
// channel, so one can keep playing while the other loads and fades in - real
// mixing done by the 3DS's own DSP (both channels feed the same output),
// not manual PCM blending on the CPU.

// Starts loading `url` (with duration_ms and normalization gain_db_to, same
// idea as audio_player_set_track_gain()) on the *other* deck, and crossfades
// to it over fade_ms while the current track keeps playing uninterrupted in
// the meantime - see audio_player_active_download_finished()'s note on
// why callers should wait for that first. Once the fade completes, the new
// track becomes "the" active track for every other function in this file
// (get_state/get_position_ms/etc.), same as an audio_player_load_url()
// would have, just arrived at gradually instead of instantly.
bool audio_player_start_crossfade(const char* url, float gain_db_to, int duration_ms, int fade_ms);

// True while a crossfade is in progress (from the start_crossfade() call
// until fade_ms has elapsed and the incoming track has taken over).
bool audio_player_is_crossfading(void);

// Aborts an in-progress crossfade: tears down the incoming deck and snaps
// the still-current track back to full volume. Call this before a manual
// skip/seek/prev so it hard-cuts cleanly instead of leaving the abandoned
// deck's stream running or the volume envelope mid-ramp - audio_player_
// load_url() already does this itself, so it's only needed directly if you
// need to cancel *without* immediately loading something else.
void audio_player_cancel_crossfade(void);

#endif // AUDIO_PLAYER_H
