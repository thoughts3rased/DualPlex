#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <mpg123.h>
#include <curl/curl.h>

#define DR_FLAC_IMPLEMENTATION
#include "lib/dr_flac.h"

#include "audio_player.h"
#include "config.h"
#include "logger.h"
#include "plex_api.h"

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_NUM_CHANNELS 2
#define AUDIO_SAMPLES_PER_BUF 4096
#define AUDIO_NUM_WAVE_BUFS 4
// How far ahead of playback the download is allowed to buffer compressed
// audio. This is the main defense against mid-track stutter: dr_flac's
// read callback (deck_flac_read_proc) has to synchronously block the whole
// app when the ring runs dry mid-decode (it can't return "try again later"
// without falsely signaling end-of-stream - see its comment). High-bitrate
// FLAC (800kbps+ is common on well-ripped CDs) can outpace a merely-adequate
// WiFi connection for stretches at a time; 256KB only banked ~2.4s of
// headroom for an 847kbps track, too little margin for a real network
// hiccup. 1MB banks ~9.7s for that same track and is trivial memory for the
// New3DS FLAC direct-stream requires anyway. Each of the two decks gets its
// own full-size ring (2MB total) rather than a smaller shared one, since
// during a crossfade both are genuinely playing back simultaneously and
// both need the same stutter protection - not just one "main" stream with a
// lightweight predownload of the other, like the old prefetch design.
#define AUDIO_RING_BUF_SIZE (1024 * 1024)
#define AUDIO_PCM_BUF_SIZE (AUDIO_SAMPLES_PER_BUF * AUDIO_NUM_CHANNELS * sizeof(s16))
#define AUDIO_INITIAL_BUFFER_BYTES (48 * 1024)
#define CROSSFADE_HALF_PI 1.57079632679489661923f

typedef struct {
    u8* data;
    size_t read_pos;
    size_t write_pos;
    bool download_finished;
    bool download_error;
    size_t total_downloaded;
} RingBuffer;

typedef enum {
    CODEC_MP3,
    CODEC_FLAC
} AudioCodec;

// One independent decode pipeline: its own decoder state, ring buffer, curl
// transfer, and NDSP channel. Two of these (s_decks[0]/s_decks[1]) exist so
// a crossfade can have both the outgoing and incoming track genuinely
// decoding and playing at once - the actual audio *mixing* is done by the
// 3DS's own DSP mixing both channels' output together, driven by each
// deck's `envelope` (the per-channel volume set via ndspChnSetMix), not by
// summing PCM samples on the CPU.
typedef struct {
    bool active;             // false = fully torn down / not in use
    int ndsp_channel;        // 0 or 1, fixed for this deck's lifetime
    AudioCodec codec;
    mpg123_handle* mpg;       // each deck needs its own - decode state can't be shared between two simultaneous streams
    drflac* flac;
    ndspWaveBuf wave_bufs[AUDIO_NUM_WAVE_BUFS];
    s16* pcm_buffer;          // linearAlloc'd, AUDIO_NUM_WAVE_BUFS slices
    u8* feed_buffer;
    RingBuffer ring;
    CURL* curl_easy;
    struct curl_slist* headers;
    bool curl_paused;
    char current_url[PLEX_MAX_URL];
    int duration_ms;
    int samples_played;
    int position_offset_ms;   // added to sample-derived position; see audio_player_set_position_offset_ms()
    // The actual sample rate currently being decoded at (mp3 is always fixed
    // at AUDIO_SAMPLE_RATE via mpg123_format(), but FLAC decodes at the
    // source's true native rate - e.g. 48000 or 96000, not necessarily
    // AUDIO_SAMPLE_RATE). Position reporting must divide by this, not the
    // constant, or the reported position (and therefore the progress bar)
    // runs fast/slow by whatever ratio the two differ by.
    float decode_sample_rate;
    bool initial_buffering;
    // Loudness-normalization gain (linear multiplier, derived from Plex's
    // Sonic Analysis dB value - see audio_player_set_track_gain()) and the
    // separate 0..1 crossfade envelope. The channel's actual volume is
    // always gain_linear * envelope; envelope is 1.0 outside of a crossfade,
    // so gain_linear alone is what "loudness leveling" this track amounts
    // to, and both trivially multiply together during an actual crossfade.
    float gain_linear;
    float gain_linear_target; // gain_linear eases toward this each frame rather than snapping - see deck_update_gain_ramp()
    float envelope;
} Deck;

static Deck s_decks[2];
static int s_active_deck = 0; // which deck is "the" track for get_state()/get_position_ms()/etc.

static PlayerState s_state = PLAYER_STOPPED;
static CURLM* s_curl_multi = NULL;

// --- Crossfade state -------------------------------------------------------
static bool s_crossfade_active = false;
static u64 s_crossfade_start_tick = 0;
static int s_crossfade_duration_ms = 0;

// --- Visualizer snapshot: most recently decoded audio, mono-downmixed ---
// Always reflects the active deck (see deck_decode_and_feed()'s is_primary
// param), even mid-crossfade - the visualizer should track whatever the user
// currently perceives as "the" track, not whichever deck happens to decode
// last in a given frame.
#define VIS_SNAPSHOT_SAMPLES 256
static s16 s_vis_snapshot[VIS_SNAPSHOT_SAMPLES];
static int s_vis_snapshot_count = 0;

// Adaptive quality: download speed measurement (active deck only - this
// drives ui.c's quality-tier suggestion for whatever's actually playing).
static u64 s_speed_window_start = 0;
static size_t s_speed_window_bytes = 0;
static int s_download_speed_bps = 0;

// Adaptive quality: buffer underrun detection (active deck only).
static int s_underrun_count = 0;
static bool s_needs_quality_downgrade = false;
static u64 s_last_underrun_tick = 0;

// --- Per-deck ring buffer -------------------------------------------------

static bool deck_ring_init(Deck* d) {
    if (!d->ring.data) {
        d->ring.data = malloc(AUDIO_RING_BUF_SIZE);
    }
    if (!d->ring.data) return false;
    d->ring.read_pos = 0;
    d->ring.write_pos = 0;
    d->ring.download_finished = false;
    d->ring.download_error = false;
    d->ring.total_downloaded = 0;
    return true;
}

static void deck_ring_free(Deck* d) {
    if (d->ring.data) {
        free(d->ring.data);
        d->ring.data = NULL;
    }
}

static size_t deck_ring_available_read(Deck* d) {
    if (!d->ring.data) return 0;
    if (d->ring.write_pos >= d->ring.read_pos) {
        return d->ring.write_pos - d->ring.read_pos;
    }
    return AUDIO_RING_BUF_SIZE - (d->ring.read_pos - d->ring.write_pos);
}

static size_t deck_ring_available_write(Deck* d) {
    if (!d->ring.data) return 0;
    return AUDIO_RING_BUF_SIZE - deck_ring_available_read(d) - 1;
}

static void deck_ring_write(Deck* d, const u8* data, size_t len) {
    if (!d->ring.data || len == 0) return;
    size_t write_pos = d->ring.write_pos;
    size_t first_chunk = AUDIO_RING_BUF_SIZE - write_pos;

    if (len <= first_chunk) {
        memcpy(d->ring.data + write_pos, data, len);
    } else {
        memcpy(d->ring.data + write_pos, data, first_chunk);
        memcpy(d->ring.data, data + first_chunk, len - first_chunk);
    }

    d->ring.write_pos = (write_pos + len) % AUDIO_RING_BUF_SIZE;
}

static void deck_ring_read(Deck* d, u8* out, size_t len) {
    if (!d->ring.data || !out || len == 0) return;
    size_t read_pos = d->ring.read_pos;
    size_t first_chunk = AUDIO_RING_BUF_SIZE - read_pos;

    if (len <= first_chunk) {
        memcpy(out, d->ring.data + read_pos, len);
    } else {
        memcpy(out, d->ring.data + read_pos, first_chunk);
        memcpy(out + first_chunk, d->ring.data, len - first_chunk);
    }

    d->ring.read_pos = (read_pos + len) % AUDIO_RING_BUF_SIZE;
}

// Downmixes the tail of a freshly-decoded s16 PCM chunk into the mono
// visualizer snapshot. channels is whatever that decode actually produced
// (tracks can be mono or stereo), not assumed to always be stereo.
static void update_vis_snapshot(const s16* pcm, u32 nframes, int channels) {
    u32 take = nframes < VIS_SNAPSHOT_SAMPLES ? nframes : VIS_SNAPSHOT_SAMPLES;
    u32 start_frame = nframes - take;
    if (channels <= 1) {
        for (u32 i = 0; i < take; i++) {
            s_vis_snapshot[i] = pcm[start_frame + i];
        }
    } else {
        for (u32 i = 0; i < take; i++) {
            s16 l = pcm[(start_frame + i) * channels + 0];
            s16 r = pcm[(start_frame + i) * channels + 1];
            s_vis_snapshot[i] = (s16)(((s32)l + (s32)r) / 2);
        }
    }
    s_vis_snapshot_count = (int)take;
}

static size_t deck_curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    Deck* d = (Deck*)userdata;
    size_t total = size * nmemb;
    size_t avail = deck_ring_available_write(d);
    if (avail < total) {
        // Must not write any of this chunk before pausing: CURL_WRITEFUNC_PAUSE
        // tells curl this callback consumed zero bytes, so it redelivers the
        // exact same chunk in full once resumed. Writing part of it now would
        // mean those bytes get written a second time on redelivery -
        // duplicating a segment of the compressed stream, which desyncs the
        // decoder's frame alignment. It resyncs by skipping forward to the
        // next valid frame, silently dropping real audio in the process -
        // audible as small chunks of the song missing, and the whole track
        // finishing early since less content actually got decoded.
        d->curl_paused = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    deck_ring_write(d, (u8*)ptr, total);
    d->ring.total_downloaded += total;
    return total;
}

static size_t deck_flac_read_proc(void* pUserData, void* pBufferOut, size_t bytesToRead) {
    Deck* d = (Deck*)pUserData;
    size_t total_read = 0;
    u8* out = (u8*)pBufferOut;

    while (total_read < bytesToRead) {
        size_t avail = deck_ring_available_read(d);
        if (avail > 0) {
            size_t chunk = (avail < (bytesToRead - total_read)) ? avail : (bytesToRead - total_read);
            deck_ring_read(d, out + total_read, chunk);
            total_read += chunk;
        } else {
            if (d->ring.download_finished || d->ring.download_error) break;

            if (s_curl_multi && d->curl_easy) {
                int running_handles = 0;
                curl_multi_perform(s_curl_multi, &running_handles);

                CURLMsg* msg;
                int msgs_left;
                while ((msg = curl_multi_info_read(s_curl_multi, &msgs_left))) {
                    if (msg->msg == CURLMSG_DONE) {
                        for (int i = 0; i < 2; i++) {
                            Deck* dd = &s_decks[i];
                            if (dd->active && msg->easy_handle == dd->curl_easy) {
                                if (msg->data.result == CURLE_OK && dd->ring.total_downloaded > 0) dd->ring.download_finished = true;
                                else dd->ring.download_error = true;
                            }
                        }
                    }
                }
            }

            if (deck_ring_available_read(d) == 0) {
                svcSleepThread(1000000LL); // 1 ms sleep to yield thread while waiting for Wi-Fi packets
                if (d->ring.download_finished || d->ring.download_error) break;
            }
        }
    }

    return total_read;
}

/* Seek stub for drflac — HTTP streams are not seekable, so always return failure. */
static drflac_bool32 deck_flac_seek_proc(void* pUserData, int offset, drflac_seek_origin origin) {
    (void)pUserData;
    (void)offset;
    (void)origin;
    return DRFLAC_FALSE;
}

/* Tell stub for drflac — not meaningful for an HTTP stream, but drflac needs a non-NULL callback. */
static drflac_bool32 deck_flac_tell_proc(void* pUserData, drflac_int64* pCursor) {
    (void)pUserData;
    (void)pCursor;
    return DRFLAC_FALSE;
}

static void deck_apply_volume(Deck* d) {
    float vol = d->gain_linear * d->envelope;
    float mix[12] = {0};
    mix[0] = vol;
    mix[1] = vol;
    ndspChnSetMix(d->ndsp_channel, mix);
}

// Tears down whatever session a deck is running (curl transfer, decoder),
// without touching its ring buffer *allocation* - that's reused across
// loads on the same deck (deck_start_stream() resets its read/write
// pointers itself right before reusing it), same as the single-deck
// original reused s_ring across tracks instead of reallocating.
static void deck_teardown(Deck* d) {
    if (s_curl_multi && d->curl_easy) {
        curl_multi_remove_handle(s_curl_multi, d->curl_easy);
        curl_easy_cleanup(d->curl_easy);
        d->curl_easy = NULL;
    }
    if (d->headers) {
        curl_slist_free_all(d->headers);
        d->headers = NULL;
    }

    ndspChnWaveBufClear(d->ndsp_channel);
    for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
        d->wave_bufs[i].status = NDSP_WBUF_DONE;
    }
    ndspChnSetPaused(d->ndsp_channel, false);

    if (d->flac) {
        drflac_close(d->flac);
        d->flac = NULL;
    }
    if (d->mpg) {
        mpg123_open_feed(d->mpg); // reset decode state for reuse
    }

    d->curl_paused = false;
    d->active = false;
}

static bool deck_start_stream(Deck* d, const char* url) {
    if (!url || !url[0]) return false;

    deck_teardown(d);

    if (!deck_ring_init(d)) {
        LOG_ERROR("Failed to allocate ring buffer for deck %d", d->ndsp_channel);
        return false;
    }

    strncpy(d->current_url, url, sizeof(d->current_url) - 1);
    d->current_url[sizeof(d->current_url) - 1] = '\0';

    d->codec = (strstr(url, ".flac") || strstr(url, ".FLAC")) ? CODEC_FLAC : CODEC_MP3;
    if (d->codec == CODEC_MP3 && d->mpg) {
        mpg123_open_feed(d->mpg);
    }

    d->samples_played = 0;
    d->position_offset_ms = 0; // callers doing a seek should call audio_player_set_position_offset_ms() right after
    d->decode_sample_rate = AUDIO_SAMPLE_RATE; // corrected once the real format is known (MP3 NEW_FORMAT / FLAC open)
    d->initial_buffering = true;
    d->gain_linear = 1.0f;
    d->gain_linear_target = 1.0f;
    d->envelope = 1.0f;

    LOG_INFO("Loading audio stream (%s) on deck %d from URL: %s",
             (d->codec == CODEC_FLAC) ? "FLAC Lossless" : "MP3", d->ndsp_channel, d->current_url);

    d->curl_easy = curl_easy_init();
    if (!d->curl_easy) {
        LOG_ERROR("Failed to initialize libcurl easy handle for deck %d!", d->ndsp_channel);
        return false;
    }

    const char* token = plex_api_get_token();
    if (token && token[0]) {
        char token_hdr[256];
        snprintf(token_hdr, sizeof(token_hdr), "X-Plex-Token: %s", token);
        d->headers = curl_slist_append(d->headers, token_hdr);
    }
    // NOTE: deliberately no X-Plex-Client-Identifier header here - PMS's universal
    // transcoder rejects the request with a 400 if that header is present at all
    // (confirmed against real server, PMS 1.43.3). It's not needed to fetch the
    // stream since the token alone authenticates the request.
    d->headers = curl_slist_append(d->headers, "X-Plex-Product: " PLEX_PRODUCT);
    d->headers = curl_slist_append(d->headers, "X-Plex-Device: Nintendo 3DS");
    // "Chrome" rather than "Nintendo 3DS": PMS's transcoder matches X-Plex-Platform
    // against its known device-profile list and 400s on anything it doesn't
    // recognize (confirmed: "Generic" itself has since stopped covering http-
    // protocol conversion in newer PMS builds - "Chrome" backs Plex Web, one of
    // Plex's own primary supported clients, and PMS keeps its transcode profile
    // current for it).
    d->headers = curl_slist_append(d->headers, "X-Plex-Platform: Chrome");

    curl_easy_setopt(d->curl_easy, CURLOPT_URL, d->current_url);
    curl_easy_setopt(d->curl_easy, CURLOPT_HTTPHEADER, d->headers);
    curl_easy_setopt(d->curl_easy, CURLOPT_WRITEFUNCTION, deck_curl_write_cb);
    curl_easy_setopt(d->curl_easy, CURLOPT_WRITEDATA, (void*)d);
    curl_easy_setopt(d->curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(d->curl_easy, CURLOPT_UNRESTRICTED_AUTH, 1L);
    curl_easy_setopt(d->curl_easy, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(d->curl_easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(d->curl_easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(d->curl_easy, CURLOPT_USERAGENT, "DualPlex/1.0 (Nintendo 3DS)");

    if (!s_curl_multi) {
        s_curl_multi = curl_multi_init();
    }
    curl_multi_add_handle(s_curl_multi, d->curl_easy);

    d->curl_paused = false;
    d->active = true;

    ndspChnWaveBufClear(d->ndsp_channel);
    for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
        d->wave_bufs[i].status = NDSP_WBUF_DONE;
    }
    ndspChnSetPaused(d->ndsp_channel, false);
    deck_apply_volume(d);

    return true;
}

// Applies underrun-detection/quality-downgrade bookkeeping and drives global
// player state (STOPPED/ERROR) - but only from the *active* deck's own
// perspective, and only when there isn't a crossfade in progress (during a
// crossfade the outgoing deck naturally running out of downloaded data right
// as the fade finishes is expected, not an underrun or a stop condition -
// see audio_player_update()'s crossfade-completion handling instead).
static void deck_check_underrun_and_completion(Deck* d, bool playing_audio) {
    bool is_primary = (d == &s_decks[s_active_deck]);

    if (!playing_audio && !d->ring.download_finished && !d->ring.download_error &&
        d->ring.total_downloaded > 0 && deck_ring_available_read(d) == 0) {
        if (is_primary) {
            u64 now = svcGetSystemTick();
            // Debounce: only count if >1 second since last underrun
            if (now - s_last_underrun_tick > 268123480ULL) {
                s_underrun_count++;
                s_last_underrun_tick = now;
                LOG_WARN("Buffer underrun detected (#%d) - download speed: %d KB/s",
                         s_underrun_count, s_download_speed_bps / 1024);

                if (s_underrun_count >= 3) {
                    s_needs_quality_downgrade = true;
                    LOG_WARN("Multiple underruns detected - requesting quality downgrade");
                }
            }
        }
        return;
    }

    if (!playing_audio && d->ring.download_finished && deck_ring_available_read(d) == 0) {
        if (is_primary && !s_crossfade_active) {
            s_state = PLAYER_STOPPED;
        }
    } else if (d->ring.download_error) {
        if (is_primary && !s_crossfade_active) {
            s_state = PLAYER_ERROR;
        }
    }
}

static void deck_decode_and_feed(Deck* d, bool is_primary) {
    if (!d->active) return;

    // Initial buffering: wait until we have enough data before starting decode
    if (d->initial_buffering) {
        if (deck_ring_available_read(d) >= AUDIO_INITIAL_BUFFER_BYTES ||
            d->ring.download_finished || d->ring.download_error) {
            d->initial_buffering = false;
            if (is_primary && s_state == PLAYER_LOADING) s_state = PLAYER_PLAYING;
            LOG_INFO("Deck %d: initial buffering complete (%d bytes ready), starting decode",
                     d->ndsp_channel, (int)deck_ring_available_read(d));
        } else {
            return; // Still buffering, don't try to decode yet
        }
    }

    if (d->codec == CODEC_MP3 && d->mpg) {
        while (deck_ring_available_read(d) > 0 && d->feed_buffer) {
            size_t avail_read = deck_ring_available_read(d);
            size_t to_read = avail_read < 4096 ? avail_read : 4096;
            deck_ring_read(d, d->feed_buffer, to_read);
            mpg123_feed(d->mpg, d->feed_buffer, to_read);
        }

        bool playing_audio = false;
        for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
            if (d->wave_bufs[i].status == NDSP_WBUF_DONE || d->wave_bufs[i].status == NDSP_WBUF_FREE) {
                size_t bytes_decoded = 0;
                int err = mpg123_read(d->mpg, (unsigned char*)d->wave_bufs[i].data_vaddr, AUDIO_PCM_BUF_SIZE, &bytes_decoded);

                if (err == MPG123_NEW_FORMAT) {
                    long rate = 44100;
                    int channels = 2, enc = 0;
                    mpg123_getformat(d->mpg, &rate, &channels, &enc);
                    ndspChnSetRate(d->ndsp_channel, (u32)rate);
                    d->decode_sample_rate = (float)rate;
                    ndspChnSetFormat(d->ndsp_channel, channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
                }

                if (bytes_decoded > 0) {
                    d->wave_bufs[i].nsamples = bytes_decoded / (sizeof(s16) * AUDIO_NUM_CHANNELS);
                    DSP_FlushDataCache(d->wave_bufs[i].data_vaddr, bytes_decoded);
                    ndspChnWaveBufAdd(d->ndsp_channel, &d->wave_bufs[i]);
                    d->samples_played += d->wave_bufs[i].nsamples;
                    if (is_primary) update_vis_snapshot((const s16*)d->wave_bufs[i].data_vaddr, d->wave_bufs[i].nsamples, AUDIO_NUM_CHANNELS);
                    playing_audio = true;
                }
            } else {
                playing_audio = true;
            }
        }

        deck_check_underrun_and_completion(d, playing_audio);
    } else if (d->codec == CODEC_FLAC) {
        if (!d->flac) {
            /* Wait for enough data to contain the full FLAC STREAMINFO header.
               A typical FLAC header + STREAMINFO is ~42 bytes, but Plex may prepend
               HTTP metadata or the file may have large VORBIS_COMMENT/PICTURE blocks.
               Buffer 64KB to be safe before attempting to parse. */
            if (deck_ring_available_read(d) >= 65536 || d->ring.download_finished) {
                d->flac = drflac_open(deck_flac_read_proc, deck_flac_seek_proc, deck_flac_tell_proc, d, NULL);
                if (d->flac) {
                    ndspChnSetRate(d->ndsp_channel, d->flac->sampleRate);
                    d->decode_sample_rate = (float)d->flac->sampleRate;
                    ndspChnSetFormat(d->ndsp_channel, d->flac->channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
                    LOG_INFO("Deck %d FLAC decoder active: %uHz, %uch, %ubit", d->ndsp_channel, d->flac->sampleRate, d->flac->channels, d->flac->bitsPerSample);
                } else {
                    LOG_ERROR("Deck %d: failed to open FLAC stream with drflac_open!", d->ndsp_channel);
                    d->ring.download_error = true;
                }
            }
        }

        if (d->flac) {
            bool playing_audio = false;
            for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
                if (d->wave_bufs[i].status == NDSP_WBUF_DONE || d->wave_bufs[i].status == NDSP_WBUF_FREE) {
                    drflac_uint64 frames_read = drflac_read_pcm_frames_s16(d->flac, AUDIO_SAMPLES_PER_BUF, (drflac_int16*)d->wave_bufs[i].data_vaddr);
                    size_t bytes_decoded = (size_t)frames_read * d->flac->channels * sizeof(s16);

                    if (bytes_decoded > 0) {
                        d->wave_bufs[i].nsamples = (u32)frames_read;
                        DSP_FlushDataCache(d->wave_bufs[i].data_vaddr, bytes_decoded);
                        ndspChnWaveBufAdd(d->ndsp_channel, &d->wave_bufs[i]);
                        d->samples_played += (int)frames_read;
                        if (is_primary) update_vis_snapshot((const s16*)d->wave_bufs[i].data_vaddr, (u32)frames_read, d->flac->channels);
                        playing_audio = true;
                    }
                } else {
                    playing_audio = true;
                }
            }

            deck_check_underrun_and_completion(d, playing_audio);
        }
    }
}

static bool deck_init_static(Deck* d, int ndsp_channel) {
    memset(d, 0, sizeof(*d));
    d->ndsp_channel = ndsp_channel;
    d->decode_sample_rate = AUDIO_SAMPLE_RATE;
    d->gain_linear = 1.0f;
    d->gain_linear_target = 1.0f;
    d->envelope = 1.0f;

    int err = MPG123_OK;
    d->mpg = mpg123_new(NULL, &err);
    if (!d->mpg) return false;

    mpg123_format_none(d->mpg);
    mpg123_format(d->mpg, AUDIO_SAMPLE_RATE, MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_open_feed(d->mpg);

    d->pcm_buffer = (s16*)linearAlloc(AUDIO_PCM_BUF_SIZE * AUDIO_NUM_WAVE_BUFS);
    if (!d->pcm_buffer) return false;

    d->feed_buffer = (u8*)malloc(4096);
    if (!d->feed_buffer) return false;

    if (!deck_ring_init(d)) return false;

    memset(d->wave_bufs, 0, sizeof(d->wave_bufs));
    for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
        d->wave_bufs[i].data_vaddr = (void*)(d->pcm_buffer + i * AUDIO_SAMPLES_PER_BUF * AUDIO_NUM_CHANNELS);
        d->wave_bufs[i].nsamples = AUDIO_SAMPLES_PER_BUF;
        d->wave_bufs[i].status = NDSP_WBUF_DONE;
    }

    ndspChnSetInterp(ndsp_channel, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ndsp_channel, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(ndsp_channel, NDSP_FORMAT_STEREO_PCM16);
    // No app-level volume control beyond the loudness-normalization gain
    // above: the 3DS's physical volume slider already attenuates audio
    // output in hardware, so a separate user-facing software volume on top
    // of it would be redundant. deck_apply_volume() mixes at gain*envelope,
    // which is 1.0*1.0 = full scale by default (no normalization data, no
    // crossfade in progress) - the slider remains the only *user* volume
    // control, same as every other 3DS application.
    deck_apply_volume(d);

    return true;
}

bool audio_player_init(void) {
    mpg123_init();

    if (!deck_init_static(&s_decks[0], 0)) return false;
    if (!deck_init_static(&s_decks[1], 1)) return false;

    s_curl_multi = curl_multi_init();

    s_active_deck = 0;
    s_crossfade_active = false;
    s_state = PLAYER_STOPPED;
    return true;
}

void audio_player_cleanup(void) {
    audio_player_stop();
    audio_player_cancel_crossfade();

    if (s_curl_multi) {
        curl_multi_cleanup(s_curl_multi);
        s_curl_multi = NULL;
    }

    for (int i = 0; i < 2; i++) {
        Deck* d = &s_decks[i];
        if (d->mpg) {
            mpg123_delete(d->mpg);
            d->mpg = NULL;
        }
        if (d->pcm_buffer) {
            linearFree(d->pcm_buffer);
            d->pcm_buffer = NULL;
        }
        if (d->feed_buffer) {
            free(d->feed_buffer);
            d->feed_buffer = NULL;
        }
        deck_ring_free(d);
    }

    mpg123_exit();
}

bool audio_player_load_url(const char* url) {
    if (!url || !url[0]) return false;

    audio_player_cancel_crossfade(); // a manual load hard-cuts, it doesn't blend

    if (!deck_start_stream(&s_decks[s_active_deck], url)) {
        s_state = PLAYER_ERROR;
        return false;
    }

    // Not PLAYER_PLAYING yet - there's no buffered audio to actually play
    // until the initial-buffering wait finishes (see deck_decode_and_feed()).
    // Reporting PLAYING immediately was why the UI never showed any
    // buffering indicator.
    s_state = PLAYER_LOADING;

    s_speed_window_start = svcGetSystemTick();
    s_speed_window_bytes = 0;
    s_underrun_count = 0;
    s_needs_quality_downgrade = false;
    s_last_underrun_tick = 0;
    return true;
}

void audio_player_play(void) {
    if (s_state == PLAYER_PAUSED) {
        s_state = PLAYER_PLAYING;
        ndspChnSetPaused(s_decks[s_active_deck].ndsp_channel, false);
        if (s_crossfade_active) ndspChnSetPaused(s_decks[1 - s_active_deck].ndsp_channel, false);
    }
}

void audio_player_pause(void) {
    if (s_state == PLAYER_PLAYING) {
        s_state = PLAYER_PAUSED;
        ndspChnSetPaused(s_decks[s_active_deck].ndsp_channel, true);
        if (s_crossfade_active) ndspChnSetPaused(s_decks[1 - s_active_deck].ndsp_channel, true);
    }
}

void audio_player_stop(void) {
    audio_player_cancel_crossfade();
    deck_teardown(&s_decks[s_active_deck]);
    s_state = PLAYER_STOPPED;
}

void audio_player_toggle(void) {
    if (s_state == PLAYER_PLAYING) {
        audio_player_pause();
    } else if (s_state == PLAYER_PAUSED) {
        audio_player_play();
    }
}

bool audio_player_start_crossfade(const char* url, float gain_db_to, int duration_ms, int fade_ms) {
    if (!url || !url[0] || fade_ms <= 0) return false;

    Deck* in_d = &s_decks[1 - s_active_deck];
    // Defensive: the inactive deck should always be idle here (crossfade
    // completion and cancellation both tear it back down), but don't leak a
    // session onto it if something unexpected left it active.
    if (in_d->active) deck_teardown(in_d);

    if (!deck_start_stream(in_d, url)) return false;

    in_d->duration_ms = duration_ms;
    in_d->gain_linear_target = powf(10.0f, gain_db_to / 20.0f);
    in_d->gain_linear = in_d->gain_linear_target; // inaudible at envelope 0 anyway - no need to ramp this one in separately
    in_d->envelope = 0.0f;
    deck_apply_volume(in_d);

    s_crossfade_active = true;
    s_crossfade_start_tick = svcGetSystemTick();
    s_crossfade_duration_ms = fade_ms;

    LOG_INFO("Crossfade starting -> %s (deck %d) over %dms, gain=%.2fdB",
             url, in_d->ndsp_channel, fade_ms, gain_db_to);
    return true;
}

bool audio_player_is_crossfading(void) {
    return s_crossfade_active;
}

void audio_player_cancel_crossfade(void) {
    if (!s_crossfade_active) return;

    Deck* in_d = &s_decks[1 - s_active_deck];
    Deck* out_d = &s_decks[s_active_deck];

    deck_teardown(in_d);
    out_d->envelope = 1.0f;
    deck_apply_volume(out_d);

    s_crossfade_active = false;
    LOG_INFO("Crossfade cancelled");
}

void audio_player_set_track_gain(float gain_db) {
    s_decks[s_active_deck].gain_linear_target = powf(10.0f, gain_db / 20.0f);
}

bool audio_player_active_download_finished(void) {
    Deck* d = &s_decks[s_active_deck];
    return d->ring.download_finished || d->ring.download_error;
}

void audio_player_update(void) {
    if (s_state == PLAYER_STOPPED || s_state == PLAYER_ERROR) return;

    // Resume any paused curl transfers once their deck's ring has room.
    for (int i = 0; i < 2; i++) {
        Deck* d = &s_decks[i];
        if (d->active && d->curl_paused && deck_ring_available_write(d) >= 64 * 1024) {
            d->curl_paused = false;
            curl_easy_pause(d->curl_easy, CURLPAUSE_CONT);
        }
    }

    bool any_downloading = false;
    for (int i = 0; i < 2; i++) {
        Deck* d = &s_decks[i];
        if (d->active && !d->ring.download_finished && !d->ring.download_error) any_downloading = true;
    }

    if (s_curl_multi && any_downloading) {
        int running_handles = 0;
        CURLMcode mres;
        int pump_count = 0;
        do {
            mres = curl_multi_perform(s_curl_multi, &running_handles);
            pump_count++;
        } while (mres == CURLM_OK && running_handles > 0 && pump_count < 8);

        for (int i = 0; i < 2; i++) {
            Deck* d = &s_decks[i];
            if (!d->active || d->ring.download_finished || d->ring.download_error) continue;

            long http_code = 0;
            curl_easy_getinfo(d->curl_easy, CURLINFO_RESPONSE_CODE, &http_code);

            if (http_code > 0 && (http_code < 200 || http_code >= 300)) {
                if (http_code >= 300 && http_code < 400) {
                    LOG_WARN("[HTTP REDIRECT %ld] Deck %d audio stream redirected for URL: %s", http_code, i, d->current_url);
                } else if (http_code == 400 || http_code == 404) {
                    LOG_ERROR("[HTTP %ld STREAM ERROR] Deck %d transcode endpoint returned %ld for URL: %s", http_code, i, http_code, d->current_url);
                    d->ring.download_error = true;
                } else if (http_code == 401 || http_code == 403) {
                    LOG_ERROR("[HTTP %ld UNAUTHORIZED/FORBIDDEN] Deck %d token or client ID rejected for URL: %s", http_code, i, d->current_url);
                    d->ring.download_error = true;
                } else {
                    LOG_ERROR("[HTTP NON-2XX RESPONSE %ld] Deck %d audio stream URL failed: %s", http_code, i, d->current_url);
                    d->ring.download_error = true;
                }
            }
        }

        if (mres != CURLM_OK) {
            LOG_ERROR("libcurl multi perform failed with code %d", (int)mres);
            for (int i = 0; i < 2; i++) {
                Deck* d = &s_decks[i];
                if (d->active && !d->ring.download_finished) d->ring.download_error = true;
            }
        } else {
            CURLMsg* msg;
            int msgs_left;
            while ((msg = curl_multi_info_read(s_curl_multi, &msgs_left))) {
                if (msg->msg != CURLMSG_DONE) continue;
                for (int i = 0; i < 2; i++) {
                    Deck* d = &s_decks[i];
                    if (d->active && msg->easy_handle == d->curl_easy) {
                        if (msg->data.result == CURLE_OK && d->ring.total_downloaded > 0) {
                            d->ring.download_finished = true;
                            LOG_INFO("Deck %d stream download finished successfully (%d total bytes)", i, (int)d->ring.total_downloaded);
                        } else if (msg->data.result != CURLE_OK) {
                            LOG_ERROR("Deck %d curl error: %d", i, (int)msg->data.result);
                            d->ring.download_error = true;
                        }
                    }
                }
            }
        }

        // Measure download speed over 2-second windows - active/primary deck only.
        Deck* primary = &s_decks[s_active_deck];
        if (primary->active) {
            u64 now = svcGetSystemTick();
            u64 elapsed_ticks = now - s_speed_window_start;
            // 268MHz tick rate: 2 seconds = 536,000,000 ticks
            if (elapsed_ticks >= 536000000ULL && primary->ring.total_downloaded > 0) {
                size_t bytes_this_window = primary->ring.total_downloaded - s_speed_window_bytes;
                double elapsed_sec = (double)elapsed_ticks / 268123480.0;
                if (elapsed_sec > 0.1) {
                    s_download_speed_bps = (int)((double)bytes_this_window / elapsed_sec);
                }
                s_speed_window_start = now;
                s_speed_window_bytes = primary->ring.total_downloaded;
            }
        }
    }

    // Decode + feed NDSP for every active deck.
    for (int i = 0; i < 2; i++) {
        Deck* d = &s_decks[i];
        if (d->active) deck_decode_and_feed(d, i == s_active_deck);
    }

    // If the crossfade target's stream failed outright, fall back to just
    // continuing the outgoing track rather than fading into dead air.
    if (s_crossfade_active && s_decks[1 - s_active_deck].ring.download_error) {
        LOG_WARN("Crossfade target stream failed - cancelling crossfade, continuing outgoing track");
        audio_player_cancel_crossfade();
    }

    // Advance the crossfade envelope, if one's in progress.
    if (s_crossfade_active) {
        Deck* out_d = &s_decks[s_active_deck];
        Deck* in_d = &s_decks[1 - s_active_deck];

        u64 now = svcGetSystemTick();
        double elapsed_ms = (double)(now - s_crossfade_start_tick) / 268123480.0 * 1000.0;
        float t = s_crossfade_duration_ms > 0 ? (float)(elapsed_ms / (double)s_crossfade_duration_ms) : 1.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;

        // Equal-power crossfade curve: keeps perceived combined loudness
        // roughly constant through the transition, unlike a plain linear
        // fade (which dips in the middle - linear has both tracks at ~50%
        // amplitude simultaneously, and combined *power* sags well below
        // either track playing alone).
        out_d->envelope = cosf(t * CROSSFADE_HALF_PI);
        in_d->envelope = sinf(t * CROSSFADE_HALF_PI);

        if (t >= 1.0f) {
            deck_teardown(out_d);
            in_d->envelope = 1.0f;
            s_active_deck = 1 - s_active_deck;
            s_crossfade_active = false;

            // Fresh track health tracking for the deck now promoted to
            // primary - same "reset everything" the old prefetch-activation
            // path did on a seamless transition.
            s_speed_window_start = svcGetSystemTick();
            s_speed_window_bytes = 0;
            s_underrun_count = 0;
            s_needs_quality_downgrade = false;
            s_last_underrun_tick = 0;
            s_state = PLAYER_PLAYING;

            LOG_INFO("Crossfade complete - deck %d is now playing", s_active_deck);
        }
    }

    // Gain ramp (loudness-normalization easing toward its target) + apply
    // the resulting volume (gain * envelope) for every active deck.
    for (int i = 0; i < 2; i++) {
        Deck* d = &s_decks[i];
        if (!d->active) continue;
        if (d->gain_linear != d->gain_linear_target) {
            d->gain_linear += (d->gain_linear_target - d->gain_linear) * 0.1f;
            if (fabsf(d->gain_linear - d->gain_linear_target) < 0.001f) d->gain_linear = d->gain_linear_target;
        }
        deck_apply_volume(d);
    }
}

PlayerState audio_player_get_state(void) {
    return s_state;
}

float audio_player_get_progress(void) {
    Deck* d = &s_decks[s_active_deck];
    if (d->duration_ms <= 0) return 0.0f;
    float p = (float)audio_player_get_position_ms() / (float)d->duration_ms;
    return (p > 1.0f) ? 1.0f : (p < 0.0f ? 0.0f : p);
}

int audio_player_get_position_ms(void) {
    Deck* d = &s_decks[s_active_deck];
    return d->position_offset_ms + (int)(((float)d->samples_played / d->decode_sample_rate) * 1000.0f);
}

void audio_player_set_duration(int duration_ms) {
    s_decks[s_active_deck].duration_ms = duration_ms;
}

void audio_player_set_position_offset_ms(int offset_ms) {
    s_decks[s_active_deck].position_offset_ms = offset_ms;
}

int audio_player_get_visualizer_samples(s16* out, int max_samples) {
    if (!out || max_samples <= 0) return 0;
    int n = s_vis_snapshot_count < max_samples ? s_vis_snapshot_count : max_samples;
    if (n > 0) memcpy(out, s_vis_snapshot, (size_t)n * sizeof(s16));
    return n;
}

int audio_player_get_download_speed(void) {
    return s_download_speed_bps;
}

bool audio_player_needs_quality_downgrade(void) {
    return s_needs_quality_downgrade;
}

void audio_player_clear_downgrade_flag(void) {
    s_needs_quality_downgrade = false;
    s_underrun_count = 0;
}
