#include <3ds.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
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
#define AUDIO_RING_BUF_SIZE (256 * 1024)
#define AUDIO_PCM_BUF_SIZE (AUDIO_SAMPLES_PER_BUF * AUDIO_NUM_CHANNELS * sizeof(s16))

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

static PlayerState s_state = PLAYER_STOPPED;
static AudioCodec s_codec = CODEC_MP3;
static mpg123_handle* s_mpg = NULL;
static drflac* s_flac = NULL;
static ndspWaveBuf s_wave_bufs[AUDIO_NUM_WAVE_BUFS];
static s16* s_pcm_buffer = NULL;
static u8* s_feed_buffer = NULL;
static RingBuffer s_ring = {0};
static CURLM* s_curl_multi = NULL;
static CURL* s_curl_easy = NULL;
static int s_duration_ms = 0;
static int s_samples_played = 0;
static float s_volume = 0.8f;
static char s_current_url[PLEX_MAX_URL] = {0};
static struct curl_slist* s_headers = NULL;

static bool s_initial_buffering = true;
#define AUDIO_INITIAL_BUFFER_BYTES (48 * 1024)

// --- Pre-fetch state ---
#define PREFETCH_RING_BUF_SIZE (128 * 1024)
static RingBuffer s_prefetch_ring = {0};
static CURL* s_prefetch_curl = NULL;
static struct curl_slist* s_prefetch_headers = NULL;
static char s_prefetch_url[PLEX_MAX_URL] = {0};
static bool s_prefetch_active = false;
static bool s_prefetch_curl_paused = false;

// Adaptive quality: download speed measurement
static u64 s_speed_window_start = 0;       // Tick when current measurement window started
static size_t s_speed_window_bytes = 0;     // Bytes downloaded in current window
static int s_download_speed_bps = 0;        // Most recent measured speed (bytes/sec)

// Adaptive quality: buffer underrun detection  
static int s_underrun_count = 0;            // Number of underruns in current playback
static bool s_needs_quality_downgrade = false;
static u64 s_last_underrun_tick = 0;        // For debouncing

static bool ring_init(void) {
    if (!s_ring.data) {
        s_ring.data = malloc(AUDIO_RING_BUF_SIZE);
    }
    if (!s_ring.data) return false;
    s_ring.read_pos = 0;
    s_ring.write_pos = 0;
    s_ring.download_finished = false;
    s_ring.download_error = false;
    s_ring.total_downloaded = 0;
    return true;
}

static void ring_free(void) {
    if (s_ring.data) {
        free(s_ring.data);
        s_ring.data = NULL;
    }
}

static size_t ring_available_read(void) {
    if (!s_ring.data) return 0;
    if (s_ring.write_pos >= s_ring.read_pos) {
        return s_ring.write_pos - s_ring.read_pos;
    }
    return AUDIO_RING_BUF_SIZE - (s_ring.read_pos - s_ring.write_pos);
}

static size_t ring_available_write(void) {
    if (!s_ring.data) return 0;
    return AUDIO_RING_BUF_SIZE - ring_available_read() - 1;
}

static void ring_write(const u8* data, size_t len) {
    if (!s_ring.data || len == 0) return;
    size_t write_pos = s_ring.write_pos;
    size_t first_chunk = AUDIO_RING_BUF_SIZE - write_pos;
    
    if (len <= first_chunk) {
        memcpy(s_ring.data + write_pos, data, len);
    } else {
        memcpy(s_ring.data + write_pos, data, first_chunk);
        memcpy(s_ring.data, data + first_chunk, len - first_chunk);
    }
    
    s_ring.write_pos = (write_pos + len) % AUDIO_RING_BUF_SIZE;
}

static void ring_read(u8* out, size_t len) {
    if (!s_ring.data || !out || len == 0) return;
    size_t read_pos = s_ring.read_pos;
    size_t first_chunk = AUDIO_RING_BUF_SIZE - read_pos;
    
    if (len <= first_chunk) {
        memcpy(out, s_ring.data + read_pos, len);
    } else {
        memcpy(out, s_ring.data + read_pos, first_chunk);
        memcpy(out + first_chunk, s_ring.data, len - first_chunk);
    }
    
    s_ring.read_pos = (read_pos + len) % AUDIO_RING_BUF_SIZE;
}

static bool s_curl_paused = false;

static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    size_t avail = ring_available_write();
    if (avail < total) {
        if (avail > 0) {
            ring_write((u8*)ptr, avail);
            s_ring.total_downloaded += avail;
        }
        s_curl_paused = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    ring_write((u8*)ptr, total);
    s_ring.total_downloaded += total;
    return total;
}

static size_t prefetch_ring_available_read(void) {
    if (!s_prefetch_ring.data) return 0;
    if (s_prefetch_ring.write_pos >= s_prefetch_ring.read_pos) {
        return s_prefetch_ring.write_pos - s_prefetch_ring.read_pos;
    }
    return PREFETCH_RING_BUF_SIZE - (s_prefetch_ring.read_pos - s_prefetch_ring.write_pos);
}

static size_t prefetch_ring_available_write(void) {
    if (!s_prefetch_ring.data) return 0;
    return PREFETCH_RING_BUF_SIZE - prefetch_ring_available_read() - 1;
}

static void prefetch_ring_write(const u8* data, size_t len) {
    if (!s_prefetch_ring.data || len == 0) return;
    size_t write_pos = s_prefetch_ring.write_pos;
    size_t first_chunk = PREFETCH_RING_BUF_SIZE - write_pos;
    if (len <= first_chunk) {
        memcpy(s_prefetch_ring.data + write_pos, data, len);
    } else {
        memcpy(s_prefetch_ring.data + write_pos, data, first_chunk);
        memcpy(s_prefetch_ring.data, data + first_chunk, len - first_chunk);
    }
    s_prefetch_ring.write_pos = (write_pos + len) % PREFETCH_RING_BUF_SIZE;
}

static size_t prefetch_curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    size_t avail = prefetch_ring_available_write();
    if (avail < total) {
        if (avail > 0) {
            prefetch_ring_write((u8*)ptr, avail);
            s_prefetch_ring.total_downloaded += avail;
        }
        s_prefetch_curl_paused = true;
        return CURL_WRITEFUNC_PAUSE;
    }
    prefetch_ring_write((u8*)ptr, total);
    s_prefetch_ring.total_downloaded += total;
    return total;
}

void audio_player_cancel_prefetch(void) {
    if (s_prefetch_curl) {
        if (s_curl_multi) {
            curl_multi_remove_handle(s_curl_multi, s_prefetch_curl);
        }
        curl_easy_cleanup(s_prefetch_curl);
        s_prefetch_curl = NULL;
    }
    if (s_prefetch_headers) {
        curl_slist_free_all(s_prefetch_headers);
        s_prefetch_headers = NULL;
    }
    s_prefetch_active = false;
    s_prefetch_curl_paused = false;
    s_prefetch_url[0] = '\0';
}

bool audio_player_prefetch_url(const char* url) {
    if (!url || !url[0]) return false;
    
    audio_player_cancel_prefetch();
    
    if (!s_prefetch_ring.data) {
        s_prefetch_ring.data = malloc(PREFETCH_RING_BUF_SIZE);
        if (!s_prefetch_ring.data) {
            LOG_ERROR("Failed to allocate prefetch ring buffer");
            return false;
        }
    }
    s_prefetch_ring.read_pos = 0;
    s_prefetch_ring.write_pos = 0;
    s_prefetch_ring.download_finished = false;
    s_prefetch_ring.download_error = false;
    s_prefetch_ring.total_downloaded = 0;
    
    strncpy(s_prefetch_url, url, sizeof(s_prefetch_url) - 1);
    s_prefetch_url[sizeof(s_prefetch_url) - 1] = '\0';
    
    s_prefetch_curl = curl_easy_init();
    if (!s_prefetch_curl) {
        LOG_ERROR("Failed to init prefetch curl handle");
        return false;
    }
    
    if (s_prefetch_headers) {
        curl_slist_free_all(s_prefetch_headers);
        s_prefetch_headers = NULL;
    }
    const char* token = plex_api_get_token();
    if (token && token[0]) {
        char token_hdr[256];
        snprintf(token_hdr, sizeof(token_hdr), "X-Plex-Token: %s", token);
        s_prefetch_headers = curl_slist_append(s_prefetch_headers, token_hdr);
    }
    s_prefetch_headers = curl_slist_append(s_prefetch_headers, "X-Plex-Client-Identifier: 3ds-plex-client");
    s_prefetch_headers = curl_slist_append(s_prefetch_headers, "X-Plex-Product: 3DS Plex Client");
    s_prefetch_headers = curl_slist_append(s_prefetch_headers, "X-Plex-Device: Nintendo 3DS");
    s_prefetch_headers = curl_slist_append(s_prefetch_headers, "X-Plex-Platform: Nintendo 3DS");
    
    curl_easy_setopt(s_prefetch_curl, CURLOPT_URL, s_prefetch_url);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_HTTPHEADER, s_prefetch_headers);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_WRITEFUNCTION, prefetch_curl_write_cb);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_UNRESTRICTED_AUTH, 1L);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(s_prefetch_curl, CURLOPT_USERAGENT, "3DS-Plex-Client/1.0 (Nintendo 3DS)");
    
    if (!s_curl_multi) {
        s_curl_multi = curl_multi_init();
    }
    curl_multi_add_handle(s_curl_multi, s_prefetch_curl);
    
    s_prefetch_active = true;
    s_prefetch_curl_paused = false;
    LOG_INFO("Pre-fetch started for URL: %s", url);
    return true;
}

bool audio_player_is_prefetch_ready(void) {
    if (!s_prefetch_active) return false;
    return prefetch_ring_available_read() >= AUDIO_INITIAL_BUFFER_BYTES || 
           s_prefetch_ring.download_finished;
}

bool audio_player_activate_prefetch(void) {
    if (!s_prefetch_active || prefetch_ring_available_read() == 0) return false;
    
    LOG_INFO("Activating pre-fetched stream (%d bytes buffered)", 
             (int)prefetch_ring_available_read());
    
    ndspChnWaveBufClear(0);
    for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
        s_wave_bufs[i].status = NDSP_WBUF_DONE;
    }
    ndspChnSetPaused(0, false);
    
    if (s_curl_easy) {
        if (s_curl_multi) {
            curl_multi_remove_handle(s_curl_multi, s_curl_easy);
        }
        curl_easy_cleanup(s_curl_easy);
        s_curl_easy = NULL;
    }
    if (s_headers) {
        curl_slist_free_all(s_headers);
        s_headers = NULL;
    }
    
    if (s_flac) {
        drflac_close(s_flac);
        s_flac = NULL;
    }
    if (s_mpg) {
        mpg123_open_feed(s_mpg);
    }
    
    s_ring.read_pos = 0;
    s_ring.write_pos = 0;
    s_ring.download_finished = s_prefetch_ring.download_finished;
    s_ring.download_error = s_prefetch_ring.download_error;
    s_ring.total_downloaded = s_prefetch_ring.total_downloaded;
    
    size_t available = prefetch_ring_available_read();
    if (available > 0) {
        size_t to_copy = available;
        if (to_copy > AUDIO_RING_BUF_SIZE - 1) to_copy = AUDIO_RING_BUF_SIZE - 1;
        
        size_t pread = s_prefetch_ring.read_pos;
        size_t first_chunk = PREFETCH_RING_BUF_SIZE - pread;
        if (to_copy <= first_chunk) {
            memcpy(s_ring.data, s_prefetch_ring.data + pread, to_copy);
        } else {
            memcpy(s_ring.data, s_prefetch_ring.data + pread, first_chunk);
            memcpy(s_ring.data + first_chunk, s_prefetch_ring.data, to_copy - first_chunk);
        }
        s_ring.write_pos = to_copy;
    }
    
    s_curl_easy = s_prefetch_curl;
    s_prefetch_curl = NULL;
    s_headers = s_prefetch_headers;
    s_prefetch_headers = NULL;
    s_curl_paused = s_prefetch_curl_paused;
    
    curl_easy_setopt(s_curl_easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    
    strncpy(s_current_url, s_prefetch_url, sizeof(s_current_url) - 1);
    s_current_url[sizeof(s_current_url) - 1] = '\0';
    
    if (strstr(s_current_url, ".flac") || strstr(s_current_url, ".FLAC")) {
        s_codec = CODEC_FLAC;
    } else {
        s_codec = CODEC_MP3;
        if (s_mpg) {
            mpg123_open_feed(s_mpg);
        }
    }
    
    s_samples_played = 0;
    
    s_speed_window_start = svcGetSystemTick();
    s_speed_window_bytes = 0;
    s_underrun_count = 0;
    s_needs_quality_downgrade = false;
    s_last_underrun_tick = 0;
    
    s_initial_buffering = (ring_available_read() < AUDIO_INITIAL_BUFFER_BYTES) && 
                          !s_ring.download_finished;
    
    s_prefetch_active = false;
    s_prefetch_curl_paused = false;
    s_prefetch_url[0] = '\0';
    
    s_state = PLAYER_PLAYING;
    ndspChnSetPaused(0, false);
    
    LOG_INFO("Pre-fetched stream activated: %s (buffered: %d bytes, initial_buffering=%d)", 
             s_current_url, (int)ring_available_read(), s_initial_buffering);
    return true;
}

bool audio_player_init(void) {
    mpg123_init();
    int err = MPG123_OK;
    s_mpg = mpg123_new(NULL, &err);
    if (!s_mpg) return false;
    
    mpg123_format_none(s_mpg);
    mpg123_format(s_mpg, AUDIO_SAMPLE_RATE, MPG123_STEREO, MPG123_ENC_SIGNED_16);
    mpg123_open_feed(s_mpg);
    
    s_pcm_buffer = (s16*)linearAlloc(AUDIO_PCM_BUF_SIZE * AUDIO_NUM_WAVE_BUFS);
    if (!s_pcm_buffer) return false;
    
    s_feed_buffer = (u8*)malloc(4096);
    if (!s_feed_buffer) return false;
    
    if (!ring_init()) return false;
    
    memset(s_wave_bufs, 0, sizeof(s_wave_bufs));
    for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
        s_wave_bufs[i].data_vaddr = (void*)(s_pcm_buffer + i * AUDIO_SAMPLES_PER_BUF * AUDIO_NUM_CHANNELS);
        s_wave_bufs[i].nsamples = AUDIO_SAMPLES_PER_BUF;
        s_wave_bufs[i].status = NDSP_WBUF_DONE;
    }
    
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
    audio_player_set_volume(s_volume);
    
    s_curl_multi = curl_multi_init();
    
    s_state = PLAYER_STOPPED;
    return true;
}

void audio_player_cleanup(void) {
    audio_player_stop();
    if (s_curl_multi) {
        curl_multi_cleanup(s_curl_multi);
        s_curl_multi = NULL;
    }
    if (s_mpg) {
        mpg123_delete(s_mpg);
        mpg123_exit();
        s_mpg = NULL;
    }
    if (s_pcm_buffer) {
        linearFree(s_pcm_buffer);
        s_pcm_buffer = NULL;
    }
    if (s_feed_buffer) {
        free(s_feed_buffer);
        s_feed_buffer = NULL;
    }
    ring_free();
    
    audio_player_cancel_prefetch();
    if (s_prefetch_ring.data) {
        free(s_prefetch_ring.data);
        s_prefetch_ring.data = NULL;
    }
}

static size_t flac_read_proc(void* pUserData, void* pBufferOut, size_t bytesToRead) {
    size_t total_read = 0;
    u8* out = (u8*)pBufferOut;
    
    while (total_read < bytesToRead) {
        size_t avail = ring_available_read();
        if (avail > 0) {
            size_t chunk = (avail < (bytesToRead - total_read)) ? avail : (bytesToRead - total_read);
            ring_read(out + total_read, chunk);
            total_read += chunk;
        } else {
            if (s_ring.download_finished || s_ring.download_error) break;
            
            if (s_curl_multi && s_curl_easy) {
                int running_handles = 0;
                curl_multi_perform(s_curl_multi, &running_handles);
                
                CURLMsg* msg;
                int msgs_left;
                while ((msg = curl_multi_info_read(s_curl_multi, &msgs_left))) {
                    if (msg->msg == CURLMSG_DONE) {
                        if (msg->easy_handle == s_curl_easy) {
                            if (msg->data.result == CURLE_OK && s_ring.total_downloaded > 0) s_ring.download_finished = true;
                            else s_ring.download_error = true;
                        } else if (s_prefetch_active && msg->easy_handle == s_prefetch_curl) {
                            if (msg->data.result == CURLE_OK && s_prefetch_ring.total_downloaded > 0) s_prefetch_ring.download_finished = true;
                            else s_prefetch_ring.download_error = true;
                        }
                    }
                }
            }
            
            if (ring_available_read() == 0) {
                svcSleepThread(1000000LL); // 1 ms sleep to yield thread while waiting for Wi-Fi packets
                if (s_ring.download_finished || s_ring.download_error) break;
            }
        }
    }
    
    return total_read;
}

/* Seek stub for drflac — HTTP streams are not seekable, so always return failure. */
static drflac_bool32 flac_seek_proc(void* pUserData, int offset, drflac_seek_origin origin) {
    (void)pUserData;
    (void)offset;
    (void)origin;
    return DRFLAC_FALSE;
}

/* Tell stub for drflac — not meaningful for an HTTP stream, but drflac needs a non-NULL callback. */
static drflac_bool32 flac_tell_proc(void* pUserData, drflac_int64* pCursor) {
    (void)pUserData;
    (void)pCursor;
    return DRFLAC_FALSE;
}


bool audio_player_load_url(const char* url) {
    if (!url || !url[0]) return false;
    
    audio_player_stop();
    
    strncpy(s_current_url, url, sizeof(s_current_url) - 1);
    
    s_ring.read_pos = 0;
    s_ring.write_pos = 0;
    s_ring.download_finished = false;
    s_ring.download_error = false;
    s_ring.total_downloaded = 0;
    s_initial_buffering = true;
    
    if (strstr(url, ".flac") || strstr(url, ".FLAC")) {
        s_codec = CODEC_FLAC;
    } else {
        s_codec = CODEC_MP3;
        if (s_mpg) {
            mpg123_open_feed(s_mpg);
        }
    }
    
    s_samples_played = 0;
    
    // Reset adaptive quality state
    s_speed_window_start = svcGetSystemTick();
    s_speed_window_bytes = 0;
    s_underrun_count = 0;
    s_needs_quality_downgrade = false;
    s_last_underrun_tick = 0;
    
    LOG_INFO("Loading audio stream (%s) from URL: %s", (s_codec == CODEC_FLAC) ? "FLAC Lossless" : "MP3", s_current_url);
    
    s_curl_easy = curl_easy_init();
    if (!s_curl_easy) {
        LOG_ERROR("Failed to initialize libcurl easy handle for audio stream!");
        s_state = PLAYER_ERROR;
        return false;
    }
    
    if (s_headers) {
        curl_slist_free_all(s_headers);
        s_headers = NULL;
    }
    
    const char* token = plex_api_get_token();
    if (token && token[0]) {
        char token_hdr[256];
        snprintf(token_hdr, sizeof(token_hdr), "X-Plex-Token: %s", token);
        s_headers = curl_slist_append(s_headers, token_hdr);
    }
    s_headers = curl_slist_append(s_headers, "X-Plex-Client-Identifier: 3ds-plex-client");
    s_headers = curl_slist_append(s_headers, "X-Plex-Product: 3DS Plex Client");
    s_headers = curl_slist_append(s_headers, "X-Plex-Device: Nintendo 3DS");
    s_headers = curl_slist_append(s_headers, "X-Plex-Platform: Nintendo 3DS");
    
    curl_easy_setopt(s_curl_easy, CURLOPT_URL, s_current_url);
    curl_easy_setopt(s_curl_easy, CURLOPT_HTTPHEADER, s_headers);
    curl_easy_setopt(s_curl_easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(s_curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_curl_easy, CURLOPT_UNRESTRICTED_AUTH, 1L);
    curl_easy_setopt(s_curl_easy, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(s_curl_easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(s_curl_easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(s_curl_easy, CURLOPT_USERAGENT, "3DS-Plex-Client/1.0 (Nintendo 3DS)");
    
    if (!s_curl_multi) {
        s_curl_multi = curl_multi_init();
    }
    curl_multi_add_handle(s_curl_multi, s_curl_easy);
    
    s_state = PLAYER_PLAYING;
    ndspChnSetPaused(0, false);
    return true;
}

void audio_player_play(void) {
    if (s_state == PLAYER_PAUSED) {
        s_state = PLAYER_PLAYING;
        ndspChnSetPaused(0, false);
    }
}

void audio_player_pause(void) {
    if (s_state == PLAYER_PLAYING) {
        s_state = PLAYER_PAUSED;
        ndspChnSetPaused(0, true);
    }
}

void audio_player_stop(void) {
    audio_player_cancel_prefetch();
    if (s_headers) {
        curl_slist_free_all(s_headers);
        s_headers = NULL;
    }
    if (s_curl_multi && s_curl_easy) {
        curl_multi_remove_handle(s_curl_multi, s_curl_easy);
        curl_easy_cleanup(s_curl_easy);
        s_curl_easy = NULL;
    }
    ndspChnWaveBufClear(0);
    for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
        s_wave_bufs[i].status = NDSP_WBUF_DONE;
    }
    ndspChnSetPaused(0, false);
    
    s_ring.read_pos = 0;
    s_ring.write_pos = 0;
    s_ring.download_finished = false;
    s_ring.download_error = false;
    
    if (s_flac) {
        drflac_close(s_flac);
        s_flac = NULL;
    }
    if (s_mpg) {
        mpg123_open_feed(s_mpg);
    }
    s_state = PLAYER_STOPPED;
}

void audio_player_toggle(void) {
    if (s_state == PLAYER_PLAYING) {
        audio_player_pause();
    } else if (s_state == PLAYER_PAUSED) {
        audio_player_play();
    }
}

void audio_player_update(void) {
    if (s_state == PLAYER_STOPPED || s_state == PLAYER_ERROR) return;
    
    if (s_curl_multi && s_curl_easy) {
        if (s_curl_paused && ring_available_write() >= 64 * 1024) {
            s_curl_paused = false;
            curl_easy_pause(s_curl_easy, CURLPAUSE_CONT);
        }
        
        if (!s_ring.download_finished || (s_prefetch_active && !s_prefetch_ring.download_finished)) {
            int running_handles = 0;
            CURLMcode mres;
            int pump_count = 0;
            do {
                mres = curl_multi_perform(s_curl_multi, &running_handles);
                pump_count++;
            } while (mres == CURLM_OK && running_handles > 0 && pump_count < 8);
            
            if (!s_ring.download_finished && !s_ring.download_error) {
                long http_code = 0;
                curl_easy_getinfo(s_curl_easy, CURLINFO_RESPONSE_CODE, &http_code);
                
                if (http_code > 0 && (http_code < 200 || http_code >= 300)) {
                    if (http_code >= 300 && http_code < 400) {
                        LOG_WARN("[HTTP REDIRECT %ld] Audio stream redirected for URL: %s", http_code, s_current_url);
                    } else if (http_code == 400 || http_code == 404) {
                        LOG_ERROR("[HTTP %ld STREAM ERROR] Transcode endpoint returned %ld for URL: %s", http_code, http_code, s_current_url);
                        s_ring.download_error = true;
                    } else if (http_code == 401 || http_code == 403) {
                        LOG_ERROR("[HTTP %ld UNAUTHORIZED/FORBIDDEN] Token or client ID rejected for URL: %s", http_code, s_current_url);
                        s_ring.download_error = true;
                    } else {
                        LOG_ERROR("[HTTP NON-2XX RESPONSE %ld] Audio stream URL failed: %s", http_code, s_current_url);
                        s_ring.download_error = true;
                    }
                }
            }
            
            if (mres != CURLM_OK) {
                LOG_ERROR("libcurl multi perform failed with code %d", (int)mres);
                if (!s_ring.download_finished) s_ring.download_error = true;
                if (s_prefetch_active) s_prefetch_ring.download_error = true;
            } else {
                CURLMsg* msg;
                int msgs_left;
                while ((msg = curl_multi_info_read(s_curl_multi, &msgs_left))) {
                    if (msg->msg == CURLMSG_DONE) {
                        if (msg->easy_handle == s_curl_easy) {
                            if (msg->data.result == CURLE_OK && s_ring.total_downloaded > 0) {
                                s_ring.download_finished = true;
                                LOG_INFO("Audio stream download finished successfully (%d total bytes)", (int)s_ring.total_downloaded);
                            } else if (msg->data.result != CURLE_OK) {
                                LOG_ERROR("Main stream curl error: %d", (int)msg->data.result);
                                s_ring.download_error = true;
                            }
                        } else if (msg->easy_handle == s_prefetch_curl) {
                            if (msg->data.result == CURLE_OK && s_prefetch_ring.total_downloaded > 0) {
                                s_prefetch_ring.download_finished = true;
                                LOG_INFO("Pre-fetch download complete (%d bytes)", (int)s_prefetch_ring.total_downloaded);
                            } else if (msg->data.result != CURLE_OK) {
                                LOG_WARN("Pre-fetch curl error: %d", (int)msg->data.result);
                                s_prefetch_ring.download_error = true;
                            }
                        }
                    }
                }
            }
            
            // Measure download speed over 2-second windows
            {
                u64 now = svcGetSystemTick();
                u64 elapsed_ticks = now - s_speed_window_start;
                // 268MHz tick rate: 2 seconds = 536,000,000 ticks
                if (elapsed_ticks >= 536000000ULL && s_ring.total_downloaded > 0) {
                    size_t bytes_this_window = s_ring.total_downloaded - s_speed_window_bytes;
                    double elapsed_sec = (double)elapsed_ticks / 268123480.0;
                    if (elapsed_sec > 0.1) {
                        s_download_speed_bps = (int)((double)bytes_this_window / elapsed_sec);
                    }
                    s_speed_window_start = now;
                    s_speed_window_bytes = s_ring.total_downloaded;
                }
            }
        }
    }
    
    // Drive pre-fetch download alongside main stream
    if (s_prefetch_active && s_prefetch_curl && !s_prefetch_ring.download_finished) {
        if (s_prefetch_curl_paused && prefetch_ring_available_write() >= 32 * 1024) {
            s_prefetch_curl_paused = false;
            curl_easy_pause(s_prefetch_curl, CURLPAUSE_CONT);
        }
    }
    
    // Initial buffering: wait until we have enough data before starting decode
    if (s_initial_buffering) {
        if (ring_available_read() >= AUDIO_INITIAL_BUFFER_BYTES || 
            s_ring.download_finished || s_ring.download_error) {
            s_initial_buffering = false;
            LOG_INFO("Initial buffering complete (%d bytes ready), starting decode", 
                     (int)ring_available_read());
        } else {
            return; // Still buffering, don't try to decode yet
        }
    }
    
    if (s_codec == CODEC_MP3 && s_mpg) {
        while (ring_available_read() > 0 && s_feed_buffer) {
            size_t avail_read = ring_available_read();
            size_t to_read = avail_read < 4096 ? avail_read : 4096;
            ring_read(s_feed_buffer, to_read);
            mpg123_feed(s_mpg, s_feed_buffer, to_read);
        }
        
        bool playing_audio = false;
        for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
            if (s_wave_bufs[i].status == NDSP_WBUF_DONE || s_wave_bufs[i].status == NDSP_WBUF_FREE) {
                size_t bytes_decoded = 0;
                int err = mpg123_read(s_mpg, (unsigned char*)s_wave_bufs[i].data_vaddr, AUDIO_PCM_BUF_SIZE, &bytes_decoded);
                
                if (err == MPG123_NEW_FORMAT) {
                    long rate = 44100;
                    int channels = 2, enc = 0;
                    mpg123_getformat(s_mpg, &rate, &channels, &enc);
                    ndspChnSetRate(0, (u32)rate);
                    ndspChnSetFormat(0, channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
                }
                
                if (bytes_decoded > 0) {
                    s_wave_bufs[i].nsamples = bytes_decoded / (sizeof(s16) * AUDIO_NUM_CHANNELS);
                    DSP_FlushDataCache(s_wave_bufs[i].data_vaddr, bytes_decoded);
                    ndspChnWaveBufAdd(0, &s_wave_bufs[i]);
                    s_samples_played += s_wave_bufs[i].nsamples;
                    playing_audio = true;
                }
            } else {
                playing_audio = true;
            }
        }
        
        // Detect buffer underruns (decoder starved while download active)
        if (!playing_audio && !s_ring.download_finished && !s_ring.download_error && 
            s_ring.total_downloaded > 0 && ring_available_read() == 0) {
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
        
        if (!playing_audio && s_ring.download_finished && ring_available_read() == 0) {
            s_state = PLAYER_STOPPED;
        } else if (s_ring.download_error) {
            s_state = PLAYER_ERROR;
        }
    } else if (s_codec == CODEC_FLAC) {
        if (!s_flac) {
            /* Wait for enough data to contain the full FLAC STREAMINFO header.
               A typical FLAC header + STREAMINFO is ~42 bytes, but Plex may prepend
               HTTP metadata or the file may have large VORBIS_COMMENT/PICTURE blocks.
               Buffer 64KB to be safe before attempting to parse. */
            if (ring_available_read() >= 65536 || s_ring.download_finished) {
                s_flac = drflac_open(flac_read_proc, flac_seek_proc, flac_tell_proc, NULL, NULL);
                if (s_flac) {
                    ndspChnSetRate(0, s_flac->sampleRate);
                    ndspChnSetFormat(0, s_flac->channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
                    LOG_INFO("⚡ N3DS Hardware FLAC Decoder Active: %uHz, %uch, %ubit", s_flac->sampleRate, s_flac->channels, s_flac->bitsPerSample);
                } else {
                    LOG_ERROR("Failed to open FLAC stream with drflac_open!");
                    s_ring.download_error = true;
                }
            }
        }
        
        if (s_flac) {
            bool playing_audio = false;
            for (int i = 0; i < AUDIO_NUM_WAVE_BUFS; i++) {
                if (s_wave_bufs[i].status == NDSP_WBUF_DONE || s_wave_bufs[i].status == NDSP_WBUF_FREE) {
                    drflac_uint64 frames_read = drflac_read_pcm_frames_s16(s_flac, AUDIO_SAMPLES_PER_BUF, (drflac_int16*)s_wave_bufs[i].data_vaddr);
                    size_t bytes_decoded = (size_t)frames_read * s_flac->channels * sizeof(s16);
                    
                    if (bytes_decoded > 0) {
                        s_wave_bufs[i].nsamples = (u32)frames_read;
                        DSP_FlushDataCache(s_wave_bufs[i].data_vaddr, bytes_decoded);
                        ndspChnWaveBufAdd(0, &s_wave_bufs[i]);
                        s_samples_played += (int)frames_read;
                        playing_audio = true;
                    }
                } else {
                    playing_audio = true;
                }
            }
            
            // Detect buffer underruns (decoder starved while download active)
            if (!playing_audio && !s_ring.download_finished && !s_ring.download_error && 
                s_ring.total_downloaded > 0 && ring_available_read() == 0) {
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
            
            if (!playing_audio && s_ring.download_finished && ring_available_read() == 0) {
                s_state = PLAYER_STOPPED;
            } else if (s_ring.download_error) {
                s_state = PLAYER_ERROR;
            }
        }
    }
}

PlayerState audio_player_get_state(void) {
    return s_state;
}

float audio_player_get_progress(void) {
    if (s_duration_ms <= 0) return 0.0f;
    float p = (float)audio_player_get_position_ms() / (float)s_duration_ms;
    return (p > 1.0f) ? 1.0f : (p < 0.0f ? 0.0f : p);
}

int audio_player_get_position_ms(void) {
    return (int)(((float)s_samples_played / (float)AUDIO_SAMPLE_RATE) * 1000.0f);
}

void audio_player_set_duration(int duration_ms) {
    s_duration_ms = duration_ms;
}

void audio_player_set_volume(float vol) {
    s_volume = vol;
    if (s_volume < 0.0f) s_volume = 0.0f;
    if (s_volume > 1.0f) s_volume = 1.0f;
    float mix[12] = {s_volume, s_volume};
    ndspChnSetMix(0, mix);
}

float audio_player_get_volume(void) {
    return s_volume;
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
