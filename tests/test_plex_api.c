/*
 * Host-run unit tests for the transcode/stream-URL logic in source/plex_api.c.
 *
 * These compile and link the REAL plex_api.c (the exact file that ships in
 * the 3DS build) against a native host compiler, with two small stand-ins
 * for the pieces plex_api.c would otherwise pull from 3DS hardware/devkitPro:
 *   - tests/stubs/3ds.h + apt_stub.c   (New3DS detection)
 *   - tests/stubs/logger_stub.c        (SD-card logging)
 * Everything else - string/URL building, cJSON, libcurl - is the same code
 * and the same libcurl that runs on the 3DS.
 *
 * Run via: make -f tests/Makefile   (see tests/README.md)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>

#include "plex_api.h"
#include "3ds.h" /* for g_stub_is_new3ds */

extern bool g_stub_is_new3ds;

/* Not part of the public API (deliberately not in plex_api.h) - given external
 * linkage in plex_api.c only so it can be exercised directly here. See its
 * definition for why. */
extern int parse_lyrics_stream_response(const char* response, PlexLyricLine* out, int max);

/* ---------------------------------------------------------------- test framework */

static int g_tests_run = 0;
static int g_tests_failed = 0;
static const char* g_current_test = "";

#define TEST(name) static void name(void)
#define RUN(name) do { \
        g_current_test = #name; \
        g_tests_run++; \
        name(); \
    } while (0)

#define CHECK(cond) do { \
        if (!(cond)) { \
            g_tests_failed++; \
            printf("  FAIL [%s] %s:%d: %s\n", g_current_test, __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define CHECK_STR_EQ(actual, expected) do { \
        const char* _a = (actual); const char* _e = (expected); \
        if (!_a || !_e || strcmp(_a, _e) != 0) { \
            g_tests_failed++; \
            printf("  FAIL [%s] %s:%d: expected \"%s\", got \"%s\"\n", \
                   g_current_test, __FILE__, __LINE__, _e ? _e : "(null)", _a ? _a : "(null)"); \
        } \
    } while (0)

/* ---------------------------------------------------------------- URL helpers */

/* Returns a malloc'd copy of query param `key`'s raw (still percent-encoded)
 * value from `url`, or NULL if the param isn't present. */
static char* query_param_raw(const char* url, const char* key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "%s=", key);

    const char* p = strstr(url, needle);
    if (!p) return NULL;
    /* Make sure we matched a real param boundary (start of query, or after '?'/'&'),
     * not a suffix of some other param name. */
    if (p != url && p[-1] != '?' && p[-1] != '&') {
        /* search again past this occurrence in case of an unlucky substring match */
        const char* next = strstr(p + 1, needle);
        while (next && next[-1] != '?' && next[-1] != '&') {
            next = strstr(next + 1, needle);
        }
        p = next;
        if (!p) return NULL;
    }

    p += strlen(needle);
    const char* end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);

    char* out = malloc(len + 1);
    memcpy(out, p, len);
    out[len] = '\0';
    return out;
}

/* Percent-decodes a raw query param value using the same libcurl the app uses. */
static char* url_decode(const char* raw) {
    CURL* curl = curl_easy_init();
    int outlen = 0;
    char* decoded = curl_easy_unescape(curl, raw, 0, &outlen);
    char* out = malloc((size_t)outlen + 1);
    memcpy(out, decoded, (size_t)outlen);
    out[outlen] = '\0';
    curl_free(decoded);
    curl_easy_cleanup(curl);
    return out;
}

static void make_track(PlexTrack* t, const char* rating_key, const char* part_key,
                        const char* audio_codec, int bitrate) {
    memset(t, 0, sizeof(*t));
    if (rating_key) snprintf(t->rating_key, sizeof(t->rating_key), "%s", rating_key);
    if (part_key) snprintf(t->part_key, sizeof(t->part_key), "%s", part_key);
    if (audio_codec) snprintf(t->audio_codec, sizeof(t->audio_codec), "%s", audio_codec);
    t->bitrate = bitrate;
}

static void init_plex_api(void) {
    plex_api_init("http://192.168.0.200:32400", "TESTTOKEN123");
    g_stub_is_new3ds = false;
}

/* Reads a fixture file relative to the repo root (tests are run via
 * `make -f tests/Makefile` from the repo root - see tests/Makefile). Returns
 * a malloc'd, NUL-terminated buffer, or NULL on failure. */
static char* read_fixture(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return NULL; }
    char* buf = malloc((size_t)size + 1);
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

/* ================================================================== */
/* Regression: FLAC_DIRECT tier must never produce audioBitrate=0 in a
 * transcode URL. QUALITY_FLAC_DIRECT means "skip transcoding entirely", but
 * plex_api_get_transcode_url() can still be reached with that tier active
 * (e.g. non-New3DS hardware, or a non-FLAC/non-MP3 source file). Sending
 * audioBitrate=0 makes Plex Media Server reject the request with a 400. */
TEST(transcode_url_flac_direct_tier_falls_back_to_320_bitrate) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_FLAC_DIRECT);

    PlexTrack track;
    make_track(&track, "12345", NULL, NULL, 0);

    char url[2048];
    CHECK(plex_api_get_transcode_url(&track, url, sizeof(url)));

    char* audio_bitrate = query_param_raw(url, "audioBitrate");
    char* max_audio_bitrate = query_param_raw(url, "maxAudioBitrate");
    CHECK_STR_EQ(audio_bitrate, "320");
    CHECK_STR_EQ(max_audio_bitrate, "320");
    free(audio_bitrate);
    free(max_audio_bitrate);
}

TEST(transcode_url_uses_selected_tier_bitrate) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_MP3_192);

    PlexTrack track;
    make_track(&track, "555", NULL, NULL, 0);

    char url[2048];
    CHECK(plex_api_get_transcode_url(&track, url, sizeof(url)));

    char* audio_bitrate = query_param_raw(url, "audioBitrate");
    CHECK_STR_EQ(audio_bitrate, "192");
    free(audio_bitrate);
}

/* ================================================================== */
/* Regression: identity/auth must ride on X-Plex-* HTTP headers (set by
 * audio_player.c), not on the transcode URL's query string. PMS 400s when
 * the same identity is asserted both ways on this endpoint. */
TEST(transcode_url_does_not_duplicate_identity_as_query_params) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_MP3_320);

    PlexTrack track;
    make_track(&track, "16324", NULL, NULL, 0);

    char url[2048];
    CHECK(plex_api_get_transcode_url(&track, url, sizeof(url)));

    CHECK(strstr(url, "X-Plex-Token=") == NULL);
    CHECK(strstr(url, "X-Plex-Client-Identifier=") == NULL);
    CHECK(strstr(url, "X-Plex-Product=") == NULL);
}

/* ================================================================== */
/* Regression: every transcode request used a single hardcoded session id.
 * The app runs two transcodes concurrently by design (current track playing
 * + next track prefetching), so a shared session id makes PMS treat the
 * second request as re-targeting the first one's session - which 400s both. */
TEST(transcode_url_session_id_is_unique_per_track) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_MP3_320);

    PlexTrack track_a, track_b;
    make_track(&track_a, "16324", NULL, NULL, 0);
    make_track(&track_b, "18309", NULL, NULL, 0);

    char url_a[2048], url_b[2048];
    CHECK(plex_api_get_transcode_url(&track_a, url_a, sizeof(url_a)));
    CHECK(plex_api_get_transcode_url(&track_b, url_b, sizeof(url_b)));

    char* session_a = query_param_raw(url_a, "session");
    char* session_b = query_param_raw(url_b, "session");

    CHECK(session_a != NULL && session_b != NULL);
    if (session_a && session_b) {
        CHECK(strcmp(session_a, session_b) != 0);
        /* Each session id should still be traceable to its own track. */
        CHECK(strstr(session_a, "16324") != NULL);
        CHECK(strstr(session_b, "18309") != NULL);
    }
    free(session_a);
    free(session_b);
}

/* ================================================================== */
/* path= must be exactly "<server>/library/metadata/<ratingKey>" (percent-
 * encoded) regardless of what form rating_key came in as. */
TEST(transcode_url_path_from_bare_rating_key) {
    init_plex_api();
    PlexTrack track;
    make_track(&track, "999", NULL, NULL, 0);

    char url[2048];
    CHECK(plex_api_get_transcode_url(&track, url, sizeof(url)));

    char* raw = query_param_raw(url, "path");
    char* decoded = raw ? url_decode(raw) : NULL;
    CHECK_STR_EQ(decoded, "http://192.168.0.200:32400/library/metadata/999");
    free(raw);
    free(decoded);
}

TEST(transcode_url_path_from_already_prefixed_rating_key) {
    init_plex_api();
    PlexTrack track;
    make_track(&track, "/library/metadata/999", NULL, NULL, 0);

    char url[2048];
    CHECK(plex_api_get_transcode_url(&track, url, sizeof(url)));

    char* raw = query_param_raw(url, "path");
    char* decoded = raw ? url_decode(raw) : NULL;
    /* Must not double up to ".../library/metadata/library/metadata/999" */
    CHECK_STR_EQ(decoded, "http://192.168.0.200:32400/library/metadata/999");
    free(raw);
    free(decoded);
}

TEST(transcode_url_falls_back_to_part_key_when_rating_key_empty) {
    init_plex_api();
    PlexTrack track;
    make_track(&track, NULL, "/library/parts/42/1/file.flac", NULL, 0);

    char url[2048];
    CHECK(plex_api_get_transcode_url(&track, url, sizeof(url)));

    char* raw = query_param_raw(url, "path");
    char* decoded = raw ? url_decode(raw) : NULL;
    CHECK_STR_EQ(decoded, "http://192.168.0.200:32400/library/parts/42/1/file.flac");
    free(raw);
    free(decoded);
}

TEST(transcode_url_fails_gracefully_with_no_rating_key_or_part_key) {
    init_plex_api();
    PlexTrack track;
    make_track(&track, NULL, NULL, NULL, 0);

    char url[2048] = {0};
    CHECK(!plex_api_get_transcode_url(&track, url, sizeof(url)));
}

/* ================================================================== */
/* plex_api_get_seek_url(): reload-from-offset seeking. */

TEST(transcode_url_defaults_to_offset_zero) {
    init_plex_api();
    PlexTrack track;
    make_track(&track, "111", NULL, NULL, 0);

    char url[2048];
    CHECK(plex_api_get_transcode_url(&track, url, sizeof(url)));

    char* offset = query_param_raw(url, "offset");
    CHECK_STR_EQ(offset, "0");
    free(offset);
}

TEST(seek_url_converts_ms_to_whole_seconds) {
    init_plex_api();
    PlexTrack track;
    make_track(&track, "222", NULL, NULL, 0);

    char url[2048];
    CHECK(plex_api_get_seek_url(&track, 90500, url, sizeof(url))); /* 90.5s -> 90s */

    char* offset = query_param_raw(url, "offset");
    CHECK_STR_EQ(offset, "90");
    free(offset);
}

TEST(seek_url_reuses_the_same_per_track_session_as_a_normal_transcode) {
    /* A seek is a reload of the SAME track, so it should keep the same
     * session id a normal transcode of that track would use (not collide
     * with it, not invent a new one) - that's what lets PMS treat it as
     * "restart my existing session at a new offset". */
    init_plex_api();
    PlexTrack track;
    make_track(&track, "333", NULL, NULL, 0);

    char normal_url[2048], seek_url[2048];
    CHECK(plex_api_get_transcode_url(&track, normal_url, sizeof(normal_url)));
    CHECK(plex_api_get_seek_url(&track, 15000, seek_url, sizeof(seek_url)));

    char* normal_session = query_param_raw(normal_url, "session");
    char* seek_session = query_param_raw(seek_url, "session");
    CHECK(normal_session != NULL && seek_session != NULL);
    if (normal_session && seek_session) {
        CHECK(strcmp(normal_session, seek_session) == 0);
    }
    free(normal_session);
    free(seek_session);
}

/* ================================================================== */
/* plex_api_get_stream_url(): direct-stream vs. transcode decision. */

TEST(stream_url_direct_streams_mp3_within_tier_bitrate) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_MP3_192);

    PlexTrack track;
    make_track(&track, "1", "/library/parts/1/1/song.mp3", "mp3", 128);

    char url[2048];
    CHECK(plex_api_get_stream_url(&track, url, sizeof(url)));
    CHECK_STR_EQ(url, "http://192.168.0.200:32400/library/parts/1/1/song.mp3");
}

TEST(stream_url_transcodes_mp3_that_exceeds_tier_bitrate) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_MP3_128);

    PlexTrack track;
    make_track(&track, "2", "/library/parts/2/1/song.mp3", "mp3", 320);

    char url[2048];
    CHECK(plex_api_get_stream_url(&track, url, sizeof(url)));
    CHECK(strstr(url, "/music/:/transcode/universal/start.mp3") != NULL);
    char* audio_bitrate = query_param_raw(url, "audioBitrate");
    CHECK_STR_EQ(audio_bitrate, "128");
    free(audio_bitrate);
}

TEST(stream_url_direct_streams_flac_on_new3ds_with_flac_direct_tier) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_FLAC_DIRECT);
    g_stub_is_new3ds = true;

    PlexTrack track;
    make_track(&track, "3", "/library/parts/3/1/song.flac", "flac", 1044);

    char url[2048];
    CHECK(plex_api_get_stream_url(&track, url, sizeof(url)));
    CHECK_STR_EQ(url, "http://192.168.0.200:32400/library/parts/3/1/song.flac");
}

/* Regression: direct-streaming a hi-res FLAC (88.2/96/176.4/192kHz - not rare
 * on well-tagged libraries) straight to the 3DS's DSP produced audibly sped
 * up, pitch-shifted playback - the DSP's actual internal mixing rate is a
 * fixed ~32728Hz, and asking it to resample from something like 96000Hz
 * pushes the ratio further than it handles cleanly. Sources above 48000Hz
 * must fall through to a transcode instead, even when every other
 * FLAC-direct condition (New3DS, tier, .flac file) is met. */
TEST(stream_url_flac_direct_declines_hi_res_source_even_on_new3ds) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_FLAC_DIRECT);
    g_stub_is_new3ds = true;

    PlexTrack track;
    make_track(&track, "5", "/library/parts/5/1/song.flac", "flac", 2995);
    track.sampling_rate = 96000;

    char url[2048];
    CHECK(plex_api_get_stream_url(&track, url, sizeof(url)));
    CHECK(strstr(url, "/music/:/transcode/universal/start.mp3") != NULL);
    CHECK(strstr(url, "song.flac") == NULL); /* must NOT be the raw direct-stream URL */
}

/* A 48000Hz source is exactly at the safe boundary and should still direct-stream. */
TEST(stream_url_flac_direct_allows_48khz_on_new3ds) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_FLAC_DIRECT);
    g_stub_is_new3ds = true;

    PlexTrack track;
    make_track(&track, "6", "/library/parts/6/1/song.flac", "flac", 1000);
    track.sampling_rate = 48000;

    char url[2048];
    CHECK(plex_api_get_stream_url(&track, url, sizeof(url)));
    CHECK_STR_EQ(url, "http://192.168.0.200:32400/library/parts/6/1/song.flac");
}

/* Regression: this is the exact path that produced the original 400 bug
 * report. On an Old 3DS (or any device, for a non-FLAC/non-MP3 file) the
 * FLAC_DIRECT tier can't direct-stream, so it must fall through to a
 * transcode - and that transcode must use a real bitrate, not 0. */
TEST(stream_url_flac_direct_tier_on_old3ds_falls_back_to_transcode_with_valid_bitrate) {
    init_plex_api();
    plex_api_set_quality_tier(QUALITY_FLAC_DIRECT);
    g_stub_is_new3ds = false;

    PlexTrack track;
    make_track(&track, "4", "/library/parts/4/1/song.flac", "flac", 1044);

    char url[2048];
    CHECK(plex_api_get_stream_url(&track, url, sizeof(url)));
    CHECK(strstr(url, "/music/:/transcode/universal/start.mp3") != NULL);

    char* audio_bitrate = query_param_raw(url, "audioBitrate");
    CHECK(audio_bitrate != NULL);
    if (audio_bitrate) CHECK(strcmp(audio_bitrate, "0") != 0);
    free(audio_bitrate);
}

/* ================================================================== */
/* parse_lyrics_stream_response(): the actual bug report - lyrics always
 * showed "No time-synced lyrics available" for every track. Root cause: the
 * code assumed the lyrics stream endpoint returned either raw LRC text or a
 * flat [{"text":...,"time":...}] array, but real Plex Media Server (1.43.x)
 * returns {"MediaContainer":{"Lyrics":[{"Line":[{"startOffset":ms,
 * "Span":[{"text":...}]}]}]}} - neither of the two shapes the parser
 * recognized, so it silently found zero lines for every track. */

TEST(parse_lyrics_stream_response_real_plex_json_shape) {
    /* Captured live from PMS 1.43.3 for Skylar Spence - "Carousel"
     * (GET /library/streams/<id> for its streamType==4 "lrc" stream). */
    char* fixture = read_fixture("tests/fixtures/carousel_lyrics_response.json");
    CHECK(fixture != NULL);
    if (!fixture) return;

    PlexLyricLine lines[128];
    int count = parse_lyrics_stream_response(fixture, lines, 128);

    CHECK(count == 51);
    if (count > 0) {
        CHECK(lines[0].time_ms == 41650);
        CHECK_STR_EQ(lines[0].text, "We're both on holiday");
    }
    if (count > 1) {
        CHECK_STR_EQ(lines[1].text, "We got no plans to make");
    }
    /* Last line, so a bad end-of-array off-by-one doesn't slip past a
     * first-line-only check. */
    if (count == 51) {
        CHECK_STR_EQ(lines[50].text, "You got me spinning like a carousel");
    }
    free(fixture);
}

TEST(parse_lyrics_stream_response_respects_max_cap) {
    char* fixture = read_fixture("tests/fixtures/carousel_lyrics_response.json");
    CHECK(fixture != NULL);
    if (!fixture) return;

    PlexLyricLine lines[5];
    int count = parse_lyrics_stream_response(fixture, lines, 5);
    CHECK(count == 5);
    free(fixture);
}

TEST(parse_lyrics_stream_response_flat_array_fallback_shape) {
    const char* response = "[{\"text\":\"Hello\",\"time\":1000},{\"text\":\"World\",\"time\":2000}]";
    PlexLyricLine lines[8];
    int count = parse_lyrics_stream_response(response, lines, 8);

    CHECK(count == 2);
    if (count == 2) {
        CHECK(lines[0].time_ms == 1000);
        CHECK_STR_EQ(lines[0].text, "Hello");
        CHECK(lines[1].time_ms == 2000);
        CHECK_STR_EQ(lines[1].text, "World");
    }
}

TEST(parse_lyrics_stream_response_raw_lrc_text_fallback_shape) {
    const char* response = "[00:41.65]We're both on holiday\n[00:47.28]We got no plans to make\n";
    PlexLyricLine lines[8];
    int count = parse_lyrics_stream_response(response, lines, 8);

    CHECK(count == 2);
    if (count == 2) {
        CHECK(lines[0].time_ms == 41650);
        CHECK_STR_EQ(lines[0].text, "We're both on holiday");
        CHECK(lines[1].time_ms == 47280);
    }
}

TEST(parse_lyrics_stream_response_no_lyrics_present_returns_zero) {
    /* A syntactically valid Plex-shaped response, just with nothing in it -
     * must return 0 (letting the caller show "no lyrics"), not crash. */
    const char* response = "{\"MediaContainer\":{\"size\":0}}";
    PlexLyricLine lines[8];
    int count = parse_lyrics_stream_response(response, lines, 8);
    CHECK(count == 0);
}

/* ================================================================== */

int main(void) {
    RUN(transcode_url_flac_direct_tier_falls_back_to_320_bitrate);
    RUN(transcode_url_uses_selected_tier_bitrate);
    RUN(transcode_url_does_not_duplicate_identity_as_query_params);
    RUN(transcode_url_session_id_is_unique_per_track);
    RUN(transcode_url_path_from_bare_rating_key);
    RUN(transcode_url_path_from_already_prefixed_rating_key);
    RUN(transcode_url_falls_back_to_part_key_when_rating_key_empty);
    RUN(transcode_url_fails_gracefully_with_no_rating_key_or_part_key);
    RUN(transcode_url_defaults_to_offset_zero);
    RUN(seek_url_converts_ms_to_whole_seconds);
    RUN(seek_url_reuses_the_same_per_track_session_as_a_normal_transcode);
    RUN(stream_url_direct_streams_mp3_within_tier_bitrate);
    RUN(stream_url_transcodes_mp3_that_exceeds_tier_bitrate);
    RUN(stream_url_direct_streams_flac_on_new3ds_with_flac_direct_tier);
    RUN(stream_url_flac_direct_declines_hi_res_source_even_on_new3ds);
    RUN(stream_url_flac_direct_allows_48khz_on_new3ds);
    RUN(stream_url_flac_direct_tier_on_old3ds_falls_back_to_transcode_with_valid_bitrate);
    RUN(parse_lyrics_stream_response_real_plex_json_shape);
    RUN(parse_lyrics_stream_response_respects_max_cap);
    RUN(parse_lyrics_stream_response_flat_array_fallback_shape);
    RUN(parse_lyrics_stream_response_raw_lrc_text_fallback_shape);
    RUN(parse_lyrics_stream_response_no_lyrics_present_returns_zero);

    printf("\nran %d tests\n", g_tests_run);
    if (g_tests_failed > 0) {
        printf("%d assertion failure(s)\n", g_tests_failed);
        return 1;
    }
    printf("All tests passed.\n");
    return 0;
}
