// Offline downloads: a persistent SD-card library (library.json + tracks/ +
// thumbs/) of tracks the user has explicitly chosen to save, plus a small
// one-at-a-time background download engine that fills it in. See offline.h
// for the public shape; this file also owns the download queue and the
// manifest's on-disk JSON format.
//
// Threading: the actual transfer (curl_multi_wait/perform + the SD card
// write) runs on a dedicated background thread (see offline_init()/
// dl_thread_func()) rather than being pumped inline on the main thread's own
// frame budget - a download competing with rendering/audio decode for main-
// thread time was the other half of the stutter problem the bandwidth
// throttle below addresses (see OFFLINE_DL_THROTTLED_BYTES_PER_SEC). Pinned
// to core 2 on New3DS when available, so it runs genuinely in parallel with
// zero main-thread cost; falls back to sharing a core via the OS scheduler
// otherwise (Old3DS, or a New3DS build without the exheader capability for
// core 2 - see thread.h's threadCreate() doc comment).
//
// Every bit of mutable state in this file (the manifest, the queue, the
// active transfer) is reachable from both the main thread (ui.c, via the
// public offline_* calls) and the download thread, so it's all guarded by a
// single lock (s_lock). curl objects specifically (s_dl_multi, s_active.easy/
// fp/headers) are additionally only ever *touched* by the download thread
// itself, even under the lock - the main thread never calls a curl function
// directly. When something the main thread does (deleting an in-progress
// download, or shutting down) needs the active transfer torn down, it just
// requests that (s_cancel_active_requested) and lets the download thread
// carry it out on its own next loop iteration, rather than reaching into
// curl objects from a foreign thread.
#include "offline.h"
#include "plex_api.h"
#include "audio_player.h"
#include "logger.h"

#include <3ds.h>
#include <curl/curl.h>
#include "lib/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

// Capacities. Kept in the same ballpark as PLEX_MAX_ITEMS (the cap used
// throughout the rest of the app for library lists) rather than trying to
// support an unbounded local library - this is a "save some favorites for a
// trip" cache, not a full mirror of a Plex server. The download queue
// (s_queue) is the one exception - it grows dynamically (see queue_ensure_
// capacity()) rather than being capped at all, after a 200+ track playlist
// download turned out to silently lose everything past a fixed 150-item cap.
#define OFFLINE_MAX_TRACKS 1000
#define OFFLINE_MAX_PLAYLISTS 30
#define OFFLINE_PLAYLIST_MAX_TRACKS 500

// Caps the active download's transfer rate while audio_player.c is actively
// streaming a track over the network (see dl_thread_func()) - otherwise the
// download happily saturates however much of the WiFi link it's given,
// competing with the stream for the same bandwidth and starving its ring
// buffer, audible as stutter. 128 KB/s (~1 Mbps) comfortably covers even a
// direct N3DS FLAC stream (typically well under that on its own) with room
// to spare on any WiFi link that can sustain both at all, while still
// making real download progress in the background rather than stalling it
// outright. No cap at all (0) once nothing's streaming over the network -
// see s_network_streaming_hint/offline_set_network_streaming_hint().
#define OFFLINE_DL_THROTTLED_BYTES_PER_SEC (128 * 1024)

// Only a direct (untranscoded) download can be resumed across a restart or a
// network drop - see start_next_download()'s comment on why a transcode
// can't. Capped rather than unlimited so a persistently unreachable server
// doesn't retry forever; backoff between attempts (retry_backoff_ticks())
// keeps a real but temporary drop from hammering it either.
#define OFFLINE_MAX_RETRIES 5

#define OFFLINE_RESUME_PATH "/3ds/dualplex/offline/resume.json"
#define OFFLINE_QUEUE_PATH "/3ds/dualplex/offline/queue.json"

// 64KB - generous headroom for curl + cJSON manifest (de)serialization on
// the background download thread (see offline_init()).
#define OFFLINE_DL_THREAD_STACK_SIZE 0x10000

// A track that has finished downloading and is recorded in library.json.
// ref_count is how many still-active download requests (standalone track,
// album, artist, or playlist) currently want this track kept around - see
// offline_queue_tracks()'s header comment. The file on disk is always at
// OFFLINE_TRACKS_DIR/<sanitized rating_key>.<ext>.
typedef struct {
    char rating_key[PLEX_MAX_STR];
    char title[PLEX_MAX_STR];
    char artist_title[PLEX_MAX_STR];
    char album_title[PLEX_MAX_STR];
    char artist_rating_key[PLEX_MAX_STR]; // may be empty for oddly-tagged tracks - see artist_identity()
    char album_rating_key[PLEX_MAX_STR];  // may be empty - see album_identity()
    char ext[8];
    char audio_codec[32];
    int bitrate;
    int sampling_rate;
    int bit_depth;
    int index;
    int duration;
    int ref_count;
    long file_size;
} OfflineTrackEntry;

typedef struct {
    char rating_key[PLEX_MAX_STR];
    char title[PLEX_MAX_STR];
    int track_count;
    char track_keys[OFFLINE_PLAYLIST_MAX_TRACKS][PLEX_MAX_STR];
} OfflinePlaylistEntry;

// A download not yet started (or waiting to be retried) - metadata copied
// out of the PlexTrack that was queued (which the caller's own array may go
// on to overwrite/reuse before this actually gets downloaded, e.g.
// navigating to a different album).
typedef struct {
    char rating_key[PLEX_MAX_STR];
    char title[PLEX_MAX_STR];
    char artist_title[PLEX_MAX_STR];
    char album_title[PLEX_MAX_STR];
    char artist_rating_key[PLEX_MAX_STR];
    char album_rating_key[PLEX_MAX_STR];
    char part_key[PLEX_MAX_URL];
    char thumb[PLEX_MAX_URL];
    char audio_codec[32];
    int bitrate;
    int sampling_rate;
    int bit_depth;
    int index;
    int duration;
    // Retry bookkeeping - reset each session (not persisted to
    // OFFLINE_RESUME_PATH; an app restart is itself a fresh set of
    // attempts). See finish_active_download()'s failure path.
    int retry_count;
    u64 retry_not_before; // svcGetSystemTick() value; don't attempt before this
} OfflineQueueItem;

typedef struct {
    bool valid;
    OfflineQueueItem item;
    char item_ext[8];
    CURL* easy;
    struct curl_slist* headers;
    FILE* fp;
    char tmp_path[512];
    char final_path[512];
    size_t bytes_done;
    curl_off_t bytes_total;
} ActiveDownload;

// --- Shared state - see the module comment at the top for the locking rules ---
static LightLock s_lock;

static OfflineTrackEntry s_tracks[OFFLINE_MAX_TRACKS];
static int s_num_tracks = 0;
static OfflinePlaylistEntry s_playlists[OFFLINE_MAX_PLAYLISTS];
static int s_num_playlists = 0;

// Grows as needed (see queue_ensure_capacity()) rather than being a fixed-
// size array - persisted to OFFLINE_QUEUE_PATH (see save_queue()/load_queue())
// so anything still waiting to download survives an app restart too, not
// just the one item actively in progress (see OFFLINE_RESUME_PATH).
static OfflineQueueItem* s_queue = NULL;
static int s_queue_count = 0;
static int s_queue_capacity = 0;

static CURLM* s_dl_multi = NULL;
static ActiveDownload s_active = {0};

// Main thread -> download thread: "cancel whatever's active and delete its
// partial file" (an explicit user delete, or offline_delete_all()) - see the
// module comment on why this is a request/flag rather than the main thread
// calling cancel_active_download() itself.
static volatile bool s_cancel_active_requested = false;

// Main thread -> download thread: is audio_player.c currently streaming
// over the network right now? See offline_set_network_streaming_hint(). A
// plain flag, not lock-protected - a torn read of a single bool costs at
// worst one loop iteration's throttle decision being stale, never a real
// correctness problem, and audio_player.c's own state is main-thread-only
// regardless, so the download thread has no other way to ask it directly.
static volatile bool s_network_streaming_hint = false;

// Download thread -> main thread: "please fetch this album's cover art" -
// see request_album_thumb_if_needed()/offline_update()'s comment on why the
// actual fetch can't happen on the download thread itself.
static bool s_pending_thumb_request = false;
static char s_pending_thumb_url[PLEX_MAX_URL] = "";
static char s_pending_thumb_path[512] = "";

static Thread s_dl_thread = NULL;
static volatile bool s_dl_thread_should_run = false;

// --- Small filesystem helpers ------------------------------------------------

static void mkdir_recursive(const char* dir) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, S_IRWXU);
            *p = '/';
        }
    }
    mkdir(tmp, S_IRWXU);
}

static void ensure_dirs(void) {
    mkdir_recursive(OFFLINE_TRACKS_DIR);
    mkdir_recursive(OFFLINE_THUMBS_DIR);
}

// Replaces anything that isn't alnum/'-'/'_'/'.' with '_' - ratingKeys and
// album/title fallbacks come straight from server metadata and end up as
// path components (see track_file_path()/album_thumb_path()), so this is a
// defensive filter against a stray '/' or other character a FAT filesystem
// (or this app's own path building) wouldn't like.
static void sanitize_component(const char* in, char* out, size_t max) {
    size_t n = 0;
    if (in) {
        for (const char* p = in; *p && n < max - 1; p++) {
            char c = *p;
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            out[n++] = ok ? c : '_';
        }
    }
    out[n] = '\0';
    if (n == 0) {
        strncpy(out, "x", max - 1);
        out[max - 1] = '\0';
    }
}

static void track_file_path(const char* rating_key, const char* ext, char* out, size_t max) {
    char comp[64];
    sanitize_component(rating_key, comp, sizeof(comp));
    snprintf(out, max, "%s/%s.%s", OFFLINE_TRACKS_DIR, comp, (ext && ext[0]) ? ext : "mp3");
}

static void album_thumb_path(const char* album_id, char* out, size_t max) {
    char comp[64];
    sanitize_component(album_id, comp, sizeof(comp));
    snprintf(out, max, "%s/%s.img", OFFLINE_THUMBS_DIR, comp);
}

static void remove_all_files_in_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        remove(path);
    }
    closedir(d);
}

// --- Manifest lookups & grouping identity ------------------------------------
// Everything below this point assumes the caller already holds s_lock,
// unless otherwise noted - these are all internal helpers, never called
// directly from outside this file.

static int find_track_index(const char* rating_key) {
    if (!rating_key || !rating_key[0]) return -1;
    for (int i = 0; i < s_num_tracks; i++) {
        if (strcmp(s_tracks[i].rating_key, rating_key) == 0) return i;
    }
    return -1;
}

static int find_playlist_index(const char* rating_key) {
    if (!rating_key || !rating_key[0]) return -1;
    for (int i = 0; i < s_num_playlists; i++) {
        if (strcmp(s_playlists[i].rating_key, rating_key) == 0) return i;
    }
    return -1;
}

static int queue_find(const char* rating_key) {
    for (int i = 0; i < s_queue_count; i++) {
        if (strcmp(s_queue[i].rating_key, rating_key) == 0) return i;
    }
    return -1;
}

// Identity used to group tracks into artists/albums for offline browsing:
// the real rating key when we have one, falling back to the display title
// for the rare track that's missing it - see the struct fields' comments.
static const char* artist_identity(const OfflineTrackEntry* e) {
    return e->artist_rating_key[0] ? e->artist_rating_key : e->artist_title;
}
static const char* album_identity(const OfflineTrackEntry* e) {
    return e->album_rating_key[0] ? e->album_rating_key : e->album_title;
}

// --- Manifest persistence (library.json) -------------------------------------

static void save_manifest(void) {
    ensure_dirs();

    cJSON* root = cJSON_CreateObject();
    cJSON* tracks = cJSON_CreateArray();
    for (int i = 0; i < s_num_tracks; i++) {
        OfflineTrackEntry* t = &s_tracks[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "rating_key", t->rating_key);
        cJSON_AddStringToObject(o, "title", t->title);
        cJSON_AddStringToObject(o, "artist_title", t->artist_title);
        cJSON_AddStringToObject(o, "album_title", t->album_title);
        cJSON_AddStringToObject(o, "artist_rating_key", t->artist_rating_key);
        cJSON_AddStringToObject(o, "album_rating_key", t->album_rating_key);
        cJSON_AddStringToObject(o, "ext", t->ext);
        cJSON_AddStringToObject(o, "audio_codec", t->audio_codec);
        cJSON_AddNumberToObject(o, "bitrate", t->bitrate);
        cJSON_AddNumberToObject(o, "sampling_rate", t->sampling_rate);
        cJSON_AddNumberToObject(o, "bit_depth", t->bit_depth);
        cJSON_AddNumberToObject(o, "index", t->index);
        cJSON_AddNumberToObject(o, "duration", t->duration);
        cJSON_AddNumberToObject(o, "ref_count", t->ref_count);
        cJSON_AddNumberToObject(o, "file_size", (double)t->file_size);
        cJSON_AddItemToArray(tracks, o);
    }
    cJSON_AddItemToObject(root, "tracks", tracks);

    cJSON* playlists = cJSON_CreateArray();
    for (int i = 0; i < s_num_playlists; i++) {
        OfflinePlaylistEntry* p = &s_playlists[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "rating_key", p->rating_key);
        cJSON_AddStringToObject(o, "title", p->title);
        cJSON* tk = cJSON_CreateArray();
        for (int j = 0; j < p->track_count; j++) {
            cJSON_AddItemToArray(tk, cJSON_CreateString(p->track_keys[j]));
        }
        cJSON_AddItemToObject(o, "tracks", tk);
        cJSON_AddItemToArray(playlists, o);
    }
    cJSON_AddItemToObject(root, "playlists", playlists);

    char* text = cJSON_PrintUnformatted(root);
    if (text) {
        FILE* f = fopen(OFFLINE_MANIFEST_PATH, "w");
        if (f) {
            fputs(text, f);
            fclose(f);
        } else {
            LOG_ERROR("offline: failed to write manifest at %s", OFFLINE_MANIFEST_PATH);
        }
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

static void load_manifest(void) {
    FILE* f = fopen(OFFLINE_MANIFEST_PATH, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOG_WARN("offline: manifest at %s failed to parse - starting empty", OFFLINE_MANIFEST_PATH);
        return;
    }

#define GETSTR(dst, jkey) do { \
        cJSON* _j = cJSON_GetObjectItem(item, jkey); \
        if (_j && cJSON_IsString(_j)) strncpy(dst, _j->valuestring, sizeof(dst) - 1); \
    } while (0)
#define GETINT(dst, jkey) do { \
        cJSON* _j = cJSON_GetObjectItem(item, jkey); \
        if (_j && cJSON_IsNumber(_j)) dst = _j->valueint; \
    } while (0)

    cJSON* tracks = cJSON_GetObjectItem(root, "tracks");
    if (tracks && cJSON_IsArray(tracks)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, tracks) {
            if (s_num_tracks >= OFFLINE_MAX_TRACKS) break;
            OfflineTrackEntry* e = &s_tracks[s_num_tracks];
            memset(e, 0, sizeof(*e));
            GETSTR(e->rating_key, "rating_key");
            GETSTR(e->title, "title");
            GETSTR(e->artist_title, "artist_title");
            GETSTR(e->album_title, "album_title");
            GETSTR(e->artist_rating_key, "artist_rating_key");
            GETSTR(e->album_rating_key, "album_rating_key");
            GETSTR(e->ext, "ext");
            GETSTR(e->audio_codec, "audio_codec");
            GETINT(e->bitrate, "bitrate");
            GETINT(e->sampling_rate, "sampling_rate");
            GETINT(e->bit_depth, "bit_depth");
            GETINT(e->index, "index");
            GETINT(e->duration, "duration");
            GETINT(e->ref_count, "ref_count");
            cJSON* fs = cJSON_GetObjectItem(item, "file_size");
            if (fs && cJSON_IsNumber(fs)) e->file_size = (long)fs->valuedouble;
            // Skip corrupt-looking entries (no key, or a ref count that
            // would mean nothing actually wants this file kept) rather than
            // carrying them forward.
            if (e->rating_key[0] && e->ref_count > 0) s_num_tracks++;
        }
    }

    cJSON* playlists = cJSON_GetObjectItem(root, "playlists");
    if (playlists && cJSON_IsArray(playlists)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, playlists) {
            if (s_num_playlists >= OFFLINE_MAX_PLAYLISTS) break;
            OfflinePlaylistEntry* p = &s_playlists[s_num_playlists];
            memset(p, 0, sizeof(*p));
            GETSTR(p->rating_key, "rating_key");
            GETSTR(p->title, "title");
            cJSON* tk = cJSON_GetObjectItem(item, "tracks");
            if (tk && cJSON_IsArray(tk)) {
                cJSON* te = NULL;
                cJSON_ArrayForEach(te, tk) {
                    if (p->track_count >= OFFLINE_PLAYLIST_MAX_TRACKS) break;
                    if (cJSON_IsString(te)) {
                        strncpy(p->track_keys[p->track_count], te->valuestring, sizeof(p->track_keys[0]) - 1);
                        p->track_count++;
                    }
                }
            }
            if (p->rating_key[0]) s_num_playlists++;
        }
    }

#undef GETSTR
#undef GETINT

    cJSON_Delete(root);
}

// --- Resume record persistence (OFFLINE_RESUME_PATH) -------------------------
// Records just enough about whichever download is currently in progress to
// re-launch the exact same request later - see start_next_download()'s
// resume-detection (which figures out how much of it is already on disk by
// checking the .part file's actual size, not anything persisted here) and
// offline_init()'s pickup of this at startup. Only ever written for a
// resumable (direct, non-transcoded) download - see is_ext_playable().

static void save_resume_record(const OfflineQueueItem* item) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "rating_key", item->rating_key);
    cJSON_AddStringToObject(o, "title", item->title);
    cJSON_AddStringToObject(o, "artist_title", item->artist_title);
    cJSON_AddStringToObject(o, "album_title", item->album_title);
    cJSON_AddStringToObject(o, "artist_rating_key", item->artist_rating_key);
    cJSON_AddStringToObject(o, "album_rating_key", item->album_rating_key);
    cJSON_AddStringToObject(o, "part_key", item->part_key);
    cJSON_AddStringToObject(o, "thumb", item->thumb);
    cJSON_AddStringToObject(o, "audio_codec", item->audio_codec);
    cJSON_AddNumberToObject(o, "bitrate", item->bitrate);
    cJSON_AddNumberToObject(o, "sampling_rate", item->sampling_rate);
    cJSON_AddNumberToObject(o, "bit_depth", item->bit_depth);
    cJSON_AddNumberToObject(o, "index", item->index);
    cJSON_AddNumberToObject(o, "duration", item->duration);

    char* text = cJSON_PrintUnformatted(o);
    if (text) {
        FILE* f = fopen(OFFLINE_RESUME_PATH, "w");
        if (f) {
            fputs(text, f);
            fclose(f);
        }
        cJSON_free(text);
    }
    cJSON_Delete(o);
}

static void clear_resume_record(void) {
    remove(OFFLINE_RESUME_PATH);
}

// Returns true (with *out_item filled in) if a valid resume record exists.
static bool load_resume_record(OfflineQueueItem* out_item) {
    FILE* f = fopen(OFFLINE_RESUME_PATH, "r");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    memset(out_item, 0, sizeof(*out_item));
#define GETSTR(dst, jkey) do { \
        cJSON* _j = cJSON_GetObjectItem(root, jkey); \
        if (_j && cJSON_IsString(_j)) strncpy(dst, _j->valuestring, sizeof(dst) - 1); \
    } while (0)
#define GETINT(dst, jkey) do { \
        cJSON* _j = cJSON_GetObjectItem(root, jkey); \
        if (_j && cJSON_IsNumber(_j)) dst = _j->valueint; \
    } while (0)
    GETSTR(out_item->rating_key, "rating_key");
    GETSTR(out_item->title, "title");
    GETSTR(out_item->artist_title, "artist_title");
    GETSTR(out_item->album_title, "album_title");
    GETSTR(out_item->artist_rating_key, "artist_rating_key");
    GETSTR(out_item->album_rating_key, "album_rating_key");
    GETSTR(out_item->part_key, "part_key");
    GETSTR(out_item->thumb, "thumb");
    GETSTR(out_item->audio_codec, "audio_codec");
    GETINT(out_item->bitrate, "bitrate");
    GETINT(out_item->sampling_rate, "sampling_rate");
    GETINT(out_item->bit_depth, "bit_depth");
    GETINT(out_item->index, "index");
    GETINT(out_item->duration, "duration");
#undef GETSTR
#undef GETINT

    cJSON_Delete(root);
    return out_item->rating_key[0] != '\0' && out_item->part_key[0] != '\0';
}

// --- Queue persistence (OFFLINE_QUEUE_PATH) -----------------------------
// The full pending queue (everything not yet started - see
// OFFLINE_RESUME_PATH for the one item actively in progress), so closing
// the app - or it crashing - doesn't lose a big batch of still-queued
// downloads, only whichever one hadn't started yet loses its head start.
// Same field set as the resume record and the same reasoning for not
// persisting retry_count/retry_not_before (a restart is a fresh set of
// attempts) - kept as separate near-duplicate code rather than factored
// together since a queue is an *array* of these and a resume record is a
// single one; sharing a helper would cost more in indirection than it saves.

static void queue_item_to_json(cJSON* arr, const OfflineQueueItem* item) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "rating_key", item->rating_key);
    cJSON_AddStringToObject(o, "title", item->title);
    cJSON_AddStringToObject(o, "artist_title", item->artist_title);
    cJSON_AddStringToObject(o, "album_title", item->album_title);
    cJSON_AddStringToObject(o, "artist_rating_key", item->artist_rating_key);
    cJSON_AddStringToObject(o, "album_rating_key", item->album_rating_key);
    cJSON_AddStringToObject(o, "part_key", item->part_key);
    cJSON_AddStringToObject(o, "thumb", item->thumb);
    cJSON_AddStringToObject(o, "audio_codec", item->audio_codec);
    cJSON_AddNumberToObject(o, "bitrate", item->bitrate);
    cJSON_AddNumberToObject(o, "sampling_rate", item->sampling_rate);
    cJSON_AddNumberToObject(o, "bit_depth", item->bit_depth);
    cJSON_AddNumberToObject(o, "index", item->index);
    cJSON_AddNumberToObject(o, "duration", item->duration);
    cJSON_AddItemToArray(arr, o);
}

static void queue_item_from_json(cJSON* item, OfflineQueueItem* out) {
    memset(out, 0, sizeof(*out));
#define GETSTR(dst, jkey) do { \
        cJSON* _j = cJSON_GetObjectItem(item, jkey); \
        if (_j && cJSON_IsString(_j)) strncpy(dst, _j->valuestring, sizeof(dst) - 1); \
    } while (0)
#define GETINT(dst, jkey) do { \
        cJSON* _j = cJSON_GetObjectItem(item, jkey); \
        if (_j && cJSON_IsNumber(_j)) dst = _j->valueint; \
    } while (0)
    GETSTR(out->rating_key, "rating_key");
    GETSTR(out->title, "title");
    GETSTR(out->artist_title, "artist_title");
    GETSTR(out->album_title, "album_title");
    GETSTR(out->artist_rating_key, "artist_rating_key");
    GETSTR(out->album_rating_key, "album_rating_key");
    GETSTR(out->part_key, "part_key");
    GETSTR(out->thumb, "thumb");
    GETSTR(out->audio_codec, "audio_codec");
    GETINT(out->bitrate, "bitrate");
    GETINT(out->sampling_rate, "sampling_rate");
    GETINT(out->bit_depth, "bit_depth");
    GETINT(out->index, "index");
    GETINT(out->duration, "duration");
#undef GETSTR
#undef GETINT
}

static void save_queue(void) {
    ensure_dirs();
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < s_queue_count; i++) queue_item_to_json(arr, &s_queue[i]);
    char* text = cJSON_PrintUnformatted(arr);
    if (text) {
        FILE* f = fopen(OFFLINE_QUEUE_PATH, "w");
        if (f) {
            fputs(text, f);
            fclose(f);
        } else {
            LOG_ERROR("offline: failed to write queue at %s", OFFLINE_QUEUE_PATH);
        }
        cJSON_free(text);
    }
    cJSON_Delete(arr);
}

// Forward-declared - defined further down with the rest of the queue
// mutators, but load_queue() (called from offline_init(), before any of
// those exist yet in file order) needs to push what it reads through the
// same capacity-growing path they use.
static void queue_push_front(const OfflineQueueItem* item);

static void load_queue(void) {
    FILE* f = fopen(OFFLINE_QUEUE_PATH, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return;
    }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOG_WARN("offline: queue at %s failed to parse - starting empty", OFFLINE_QUEUE_PATH);
        return;
    }
    if (cJSON_IsArray(root)) {
        // Appended in order via repeated queue_push_front() calls would
        // reverse it, so build it back to front instead - the net result
        // is the same relative order it was saved in.
        int n = cJSON_GetArraySize(root);
        for (int i = n - 1; i >= 0; i--) {
            OfflineQueueItem qi;
            queue_item_from_json(cJSON_GetArrayItem(root, i), &qi);
            if (qi.rating_key[0] && qi.part_key[0]) queue_push_front(&qi);
        }
        LOG_INFO("offline: restored %d queued download(s) from a previous session", s_queue_count);
    }
    cJSON_Delete(root);
}

// --- Queueing ----------------------------------------------------------------

// Grows s_queue (realloc) so it can hold at least `min_capacity` items - the
// download queue has no fixed cap (unlike the rest of this file's arrays;
// see the capacities comment up top) since a large playlist download is
// exactly the kind of thing that needs more room than any fixed guess would
// give it. Failure (out of memory) just leaves the array as it was; callers
// re-check s_queue_count < s_queue_capacity before writing into it.
static void queue_ensure_capacity(int min_capacity) {
    if (s_queue_capacity >= min_capacity) return;
    int new_cap = s_queue_capacity > 0 ? s_queue_capacity * 2 : 64;
    while (new_cap < min_capacity) new_cap *= 2;
    OfflineQueueItem* new_arr = (OfflineQueueItem*)realloc(s_queue, (size_t)new_cap * sizeof(OfflineQueueItem));
    if (!new_arr) {
        LOG_ERROR("offline: out of memory growing the download queue to %d entries", new_cap);
        return;
    }
    s_queue = new_arr;
    s_queue_capacity = new_cap;
}

static void queue_push(const PlexTrack* t) {
    queue_ensure_capacity(s_queue_count + 1);
    if (s_queue_count >= s_queue_capacity) return; // out of memory - see queue_ensure_capacity()
    OfflineQueueItem* q = &s_queue[s_queue_count++];
    memset(q, 0, sizeof(*q));
    strncpy(q->rating_key, t->rating_key, sizeof(q->rating_key) - 1);
    strncpy(q->title, t->title, sizeof(q->title) - 1);
    strncpy(q->artist_title, t->grandparent_title, sizeof(q->artist_title) - 1);
    strncpy(q->album_title, t->parent_title, sizeof(q->album_title) - 1);
    strncpy(q->artist_rating_key, t->artist_rating_key, sizeof(q->artist_rating_key) - 1);
    strncpy(q->album_rating_key, t->album_rating_key, sizeof(q->album_rating_key) - 1);
    strncpy(q->part_key, t->part_key, sizeof(q->part_key) - 1);
    strncpy(q->thumb, t->thumb, sizeof(q->thumb) - 1);
    strncpy(q->audio_codec, t->audio_codec, sizeof(q->audio_codec) - 1);
    q->bitrate = t->bitrate;
    q->sampling_rate = t->sampling_rate;
    q->bit_depth = t->bit_depth;
    q->index = t->index;
    q->duration = t->duration;
}

// Pushes `item` to the FRONT of the queue - used for a retry (keep making
// progress on the thing that's already partway done before starting
// something new), for the resume record picked up at startup, and by
// load_queue() (same reasoning as the resume record for all three).
static void queue_push_front(const OfflineQueueItem* item) {
    queue_ensure_capacity(s_queue_count + 1);
    if (s_queue_count >= s_queue_capacity) {
        LOG_ERROR("offline: out of memory - dropping '%s'", item->title);
        return;
    }
    memmove(&s_queue[1], &s_queue[0], (size_t)s_queue_count * sizeof(OfflineQueueItem));
    s_queue[0] = *item;
    s_queue_count++;
}

// Actual implementation of offline_queue_tracks() - assumes the lock is
// already held, so offline_queue_playlist() below can call this directly
// instead of going through the public, lock-acquiring offline_queue_tracks()
// (LightLock isn't recursive - see the module comment). Persists the queue
// itself (not just the manifest) when anything was actually added - see
// OFFLINE_QUEUE_PATH.
static int queue_tracks_locked(const PlexTrack* tracks, int count) {
    if (!tracks || count <= 0) return 0;
    int newly_queued = 0;
    bool dirty = false;

    for (int i = 0; i < count; i++) {
        const PlexTrack* t = &tracks[i];
        if (!t->rating_key[0] || !t->part_key[0]) continue;

        int ti = find_track_index(t->rating_key);
        if (ti >= 0) {
            s_tracks[ti].ref_count++;
            dirty = true;
            continue;
        }
        // Already waiting in the queue from an earlier call - see this
        // function's header comment on why a second request while it's
        // still pending doesn't bump anything further.
        if (queue_find(t->rating_key) >= 0) continue;

        queue_push(t);
        newly_queued++;
    }

    if (dirty) save_manifest();
    if (newly_queued > 0) save_queue();
    return newly_queued;
}

int offline_queue_tracks(const PlexTrack* tracks, int count) {
    LightLock_Lock(&s_lock);
    int result = queue_tracks_locked(tracks, count);
    LightLock_Unlock(&s_lock);
    return result;
}

int offline_queue_playlist(const char* playlist_rating_key, const char* playlist_title,
                            const PlexTrack* tracks, int count) {
    if (!playlist_rating_key || !playlist_rating_key[0]) return 0;

    LightLock_Lock(&s_lock);
    int newly = queue_tracks_locked(tracks, count);

    int pi = find_playlist_index(playlist_rating_key);
    if (pi < 0) {
        if (s_num_playlists >= OFFLINE_MAX_PLAYLISTS) {
            LOG_WARN("offline: playlist manifest full (%d) - can't add '%s'",
                     OFFLINE_MAX_PLAYLISTS, playlist_title ? playlist_title : "?");
            LightLock_Unlock(&s_lock);
            return newly;
        }
        pi = s_num_playlists++;
    }

    OfflinePlaylistEntry* p = &s_playlists[pi];
    memset(p, 0, sizeof(*p));
    strncpy(p->rating_key, playlist_rating_key, sizeof(p->rating_key) - 1);
    strncpy(p->title, playlist_title ? playlist_title : "", sizeof(p->title) - 1);
    int n = count < OFFLINE_PLAYLIST_MAX_TRACKS ? count : OFFLINE_PLAYLIST_MAX_TRACKS;
    if (count > OFFLINE_PLAYLIST_MAX_TRACKS) {
        LOG_WARN("offline: playlist '%s' has %d tracks, only remembering the first %d as part of it "
                 "(all %d still get downloaded/counted individually)",
                 playlist_title ? playlist_title : "?", count, OFFLINE_PLAYLIST_MAX_TRACKS, count);
    }
    for (int i = 0; i < n; i++) {
        if (!tracks[i].rating_key[0]) continue;
        strncpy(p->track_keys[p->track_count], tracks[i].rating_key, sizeof(p->track_keys[0]) - 1);
        p->track_count++;
    }

    save_manifest();
    LightLock_Unlock(&s_lock);
    return newly;
}

// --- Download engine -----------------------------------------------------
// Everything from here down to dl_thread_func() runs exclusively on the
// background download thread (see offline_init()), with the lock already
// held by the caller unless noted otherwise.

static void derive_ext(const char* part_key, char* out, size_t max) {
    const char* base = strrchr(part_key, '/');
    base = base ? base + 1 : part_key;
    const char* dot = strrchr(base, '.');
    if (dot && dot[1]) {
        size_t n = 0;
        for (const char* p = dot + 1; *p && n < max - 1; p++) {
            char c = *p;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) break;
            out[n++] = (char)tolower((unsigned char)c);
        }
        out[n] = '\0';
        if (n >= 2 && n <= 5) return;
    }
    strncpy(out, "mp3", max - 1);
    out[max - 1] = '\0';
}

// Whether this app's audio_player.c can actually decode a file with this
// extension (mpg123 for MP3, dr_flac for FLAC - nothing else). A source file
// in any other format (AAC/M4A/OGG/WMA, not uncommon in some libraries) gets
// transcoded to MP3 at download time instead of saved as-is - see
// start_next_download() - so it's guaranteed playable offline. Also exactly
// the set of downloads that can be resumed (see the module's resume-record
// comment): a transcode is a live server-side re-encode, not a stable byte
// stream - re-issuing the same transcode request and skipping N bytes via
// Range doesn't reliably land back on the same content, so an interrupted
// transcode just starts over instead of resuming.
static bool is_ext_playable(const char* ext) {
    return strcasecmp(ext, "mp3") == 0 || strcasecmp(ext, "flac") == 0;
}

static size_t dl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    ActiveDownload* a = (ActiveDownload*)userdata;
    size_t total = size * nmemb;
    if (!a->fp) return 0;
    size_t written = fwrite(ptr, 1, total, a->fp);
    a->bytes_done += written;
    return written;
}

static void fill_plex_track_from_entry(const OfflineTrackEntry* e, PlexTrack* out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->rating_key, e->rating_key, sizeof(out->rating_key) - 1);
    strncpy(out->title, e->title, sizeof(out->title) - 1);
    strncpy(out->grandparent_title, e->artist_title, sizeof(out->grandparent_title) - 1);
    strncpy(out->parent_title, e->album_title, sizeof(out->parent_title) - 1);
    strncpy(out->artist_rating_key, e->artist_rating_key, sizeof(out->artist_rating_key) - 1);
    strncpy(out->album_rating_key, e->album_rating_key, sizeof(out->album_rating_key) - 1);
    strncpy(out->audio_codec, e->audio_codec, sizeof(out->audio_codec) - 1);
    out->bitrate = e->bitrate;
    out->sampling_rate = e->sampling_rate;
    out->bit_depth = e->bit_depth;
    out->index = e->index;
    out->duration = e->duration;
}

static void add_track_manifest_entry(const OfflineQueueItem* q, const char* ext,
                                      const char* stored_codec, int stored_bitrate, long file_size) {
    int ti = find_track_index(q->rating_key);
    if (ti < 0) {
        if (s_num_tracks >= OFFLINE_MAX_TRACKS) {
            LOG_ERROR("offline: track manifest full (%d) - can't record '%s'", OFFLINE_MAX_TRACKS, q->title);
            return;
        }
        ti = s_num_tracks++;
        memset(&s_tracks[ti], 0, sizeof(s_tracks[ti]));
    }
    OfflineTrackEntry* e = &s_tracks[ti];
    strncpy(e->rating_key, q->rating_key, sizeof(e->rating_key) - 1);
    strncpy(e->title, q->title, sizeof(e->title) - 1);
    strncpy(e->artist_title, q->artist_title, sizeof(e->artist_title) - 1);
    strncpy(e->album_title, q->album_title, sizeof(e->album_title) - 1);
    strncpy(e->artist_rating_key, q->artist_rating_key, sizeof(e->artist_rating_key) - 1);
    strncpy(e->album_rating_key, q->album_rating_key, sizeof(e->album_rating_key) - 1);
    strncpy(e->ext, ext, sizeof(e->ext) - 1);
    strncpy(e->audio_codec, stored_codec, sizeof(e->audio_codec) - 1);
    e->bitrate = stored_bitrate;
    e->sampling_rate = q->sampling_rate;
    e->bit_depth = q->bit_depth;
    e->index = q->index;
    e->duration = q->duration;
    e->file_size = file_size;
    e->ref_count += 1;
}

// Flags this track's album for a cover-art fetch next time offline_update()
// runs on the main thread, if it isn't already cached - see the module
// comment on why the download thread can't just do this fetch itself
// (plex_api_get_album_art() blocks via a helper that pumps
// audio_player_update(), which must stay on the main thread). Only one
// request is kept in flight at a time; a second album finishing before the
// main thread services the first just skips its own thumbnail this round
// (cheap, cosmetic-only, and picked up next time that album's tracks are
// touched by a fresh download anyway).
static void request_album_thumb_if_needed(const OfflineQueueItem* item) {
    if (!item->thumb[0] || s_pending_thumb_request) return;
    const char* album_id = item->album_rating_key[0] ? item->album_rating_key
                          : (item->album_title[0] ? item->album_title : item->rating_key);

    char path[512];
    album_thumb_path(album_id, path, sizeof(path));

    FILE* existing = fopen(path, "rb");
    if (existing) {
        fclose(existing);
        return;
    }

    strncpy(s_pending_thumb_url, item->thumb, sizeof(s_pending_thumb_url) - 1);
    strncpy(s_pending_thumb_path, path, sizeof(s_pending_thumb_path) - 1);
    s_pending_thumb_request = true;
}

// Tears down whatever the active transfer's curl/file handles are.
// `delete_partial` distinguishes an explicit cancel (user deleted this item,
// or offline_delete_all() wiped everything - remove the .part file and its
// resume record too) from a graceful pause (app shutting down - leave both
// in place so offline_init() picks this same download back up next launch).
static void cancel_active_download(bool delete_partial) {
    if (!s_active.valid) return;
    if (s_active.fp) {
        fclose(s_active.fp);
        s_active.fp = NULL;
    }
    if (s_dl_multi && s_active.easy) {
        curl_multi_remove_handle(s_dl_multi, s_active.easy);
        curl_easy_cleanup(s_active.easy);
        s_active.easy = NULL;
    }
    if (s_active.headers) {
        curl_slist_free_all(s_active.headers);
        s_active.headers = NULL;
    }
    if (delete_partial) {
        remove(s_active.tmp_path);
        clear_resume_record();
    }
    memset(&s_active, 0, sizeof(s_active));
}

static void start_next_download(void) {
    if (s_queue_count <= 0) return;

    // Front-most item that isn't still backing off after a failed attempt
    // (see finish_active_download()) - usually just index 0, but a retry
    // waiting out its backoff shouldn't block everything queued behind it
    // that's perfectly able to start right now.
    u64 now = svcGetSystemTick();
    int idx = -1;
    for (int i = 0; i < s_queue_count; i++) {
        if (s_queue[i].retry_not_before <= now) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return; // everything currently queued is backing off

    OfflineQueueItem item = s_queue[idx];
    memmove(&s_queue[idx], &s_queue[idx + 1], (size_t)(s_queue_count - 1 - idx) * sizeof(OfflineQueueItem));
    s_queue_count--;
    save_queue(); // reflect the pop on disk regardless of what happens to `item` next - see OFFLINE_QUEUE_PATH

    // May have become redundant while it was waiting its turn (queued twice
    // via two different calls before either one started downloading).
    int existing = find_track_index(item.rating_key);
    if (existing >= 0) {
        s_tracks[existing].ref_count++;
        save_manifest();
        return;
    }

    char ext[8];
    derive_ext(item.part_key, ext, sizeof(ext));
    bool direct = is_ext_playable(ext);

    PlexTrack stub;
    memset(&stub, 0, sizeof(stub));
    strncpy(stub.rating_key, item.rating_key, sizeof(stub.rating_key) - 1);
    strncpy(stub.part_key, item.part_key, sizeof(stub.part_key) - 1);

    char url[PLEX_MAX_URL];
    bool got_url = direct ? plex_api_get_download_url(&stub, url, sizeof(url))
                           : plex_api_get_transcode_url(&stub, url, sizeof(url));
    if (!got_url) {
        LOG_ERROR("offline: couldn't build a download URL for '%s' - skipping", item.title);
        return;
    }
    if (!direct) strncpy(ext, "mp3", sizeof(ext) - 1); // a transcode always comes back as MP3

    ensure_dirs();
    track_file_path(item.rating_key, ext, s_active.final_path, sizeof(s_active.final_path));
    snprintf(s_active.tmp_path, sizeof(s_active.tmp_path), "%s.part", s_active.final_path);

    // Resume detection: a direct download's .part file from an earlier
    // attempt (this session's retry, or picked up fresh from
    // OFFLINE_RESUME_PATH at startup) is trustworthy exactly as far as its
    // own size on disk says it is - ask the server to continue from there
    // instead of re-downloading it. A transcode never resumes (see
    // is_ext_playable()'s comment) - if a stray .part happens to exist for
    // one anyway (shouldn't, but be defensive), start clean over it.
    long existing_size = 0;
    if (direct) {
        FILE* probe = fopen(s_active.tmp_path, "rb");
        if (probe) {
            fseek(probe, 0, SEEK_END);
            existing_size = ftell(probe);
            fclose(probe);
        }
    }

    s_active.fp = fopen(s_active.tmp_path, existing_size > 0 ? "ab" : "wb");
    if (!s_active.fp) {
        LOG_ERROR("offline: couldn't open '%s' for writing", s_active.tmp_path);
        return;
    }

    s_active.easy = curl_easy_init();
    if (!s_active.easy) {
        fclose(s_active.fp);
        s_active.fp = NULL;
        return;
    }

    const char* token = plex_api_get_token();
    if (token && token[0]) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "X-Plex-Token: %s", token);
        s_active.headers = curl_slist_append(s_active.headers, hdr);
    }
    s_active.headers = curl_slist_append(s_active.headers, "X-Plex-Product: " PLEX_PRODUCT);
    s_active.headers = curl_slist_append(s_active.headers, "X-Plex-Device: Nintendo 3DS");
    s_active.headers = curl_slist_append(s_active.headers, "X-Plex-Platform: Chrome");

    curl_easy_setopt(s_active.easy, CURLOPT_URL, url);
    curl_easy_setopt(s_active.easy, CURLOPT_HTTPHEADER, s_active.headers);
    curl_easy_setopt(s_active.easy, CURLOPT_WRITEFUNCTION, dl_write_cb);
    curl_easy_setopt(s_active.easy, CURLOPT_WRITEDATA, (void*)&s_active);
    curl_easy_setopt(s_active.easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_active.easy, CURLOPT_UNRESTRICTED_AUTH, 1L);
    curl_easy_setopt(s_active.easy, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(s_active.easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(s_active.easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(s_active.easy, CURLOPT_USERAGENT, "DualPlex/1.0 (Nintendo 3DS)");
    if (existing_size > 0) {
        curl_easy_setopt(s_active.easy, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)existing_size);
    }

    if (!s_dl_multi) s_dl_multi = curl_multi_init();
    curl_multi_add_handle(s_dl_multi, s_active.easy);

    s_active.item = item;
    strncpy(s_active.item_ext, ext, sizeof(s_active.item_ext) - 1);
    s_active.bytes_done = (size_t)existing_size;
    s_active.bytes_total = 0;
    s_active.valid = true;

    if (direct) save_resume_record(&item);

    if (existing_size > 0) {
        LOG_INFO("offline: resuming '%s' from %ld bytes -> %s", item.title, existing_size, s_active.final_path);
    } else {
        LOG_INFO("offline: downloading '%s' -> %s", item.title, s_active.final_path);
    }
}

// 5s, 10s, 20s, 40s, 80s, capped at 120s - short enough that a real blip
// recovers quickly, long enough that a genuinely unreachable server isn't
// hammered every frame's worth of retries.
static u64 retry_backoff_ticks(int retry_count) {
    int shift = retry_count < 5 ? retry_count : 5;
    double secs = 5.0 * (double)(1 << shift);
    if (secs > 120.0) secs = 120.0;
    return (u64)(secs * 268123480.0); // 3DS tick rate - matches the constant already used elsewhere (e.g. audio_player.c)
}

static void finish_active_download(void) {
    bool ok = false;
    CURLMsg* msg;
    int msgs_left;
    while ((msg = curl_multi_info_read(s_dl_multi, &msgs_left)) != NULL) {
        if (msg->msg == CURLMSG_DONE && msg->easy_handle == s_active.easy) {
            ok = (msg->data.result == CURLE_OK);
        }
    }
    long http_code = 0;
    if (s_active.easy) curl_easy_getinfo(s_active.easy, CURLINFO_RESPONSE_CODE, &http_code);
    // A resumed request answers 206 Partial Content, not 200 - both mean success here.
    ok = ok && (http_code == 200 || http_code == 206) && s_active.bytes_done > 0;

    if (s_active.fp) {
        fclose(s_active.fp);
        s_active.fp = NULL;
    }
    if (s_dl_multi && s_active.easy) {
        curl_multi_remove_handle(s_dl_multi, s_active.easy);
        curl_easy_cleanup(s_active.easy);
        s_active.easy = NULL;
    }
    if (s_active.headers) {
        curl_slist_free_all(s_active.headers);
        s_active.headers = NULL;
    }

    if (ok) {
        remove(s_active.final_path); // clear out any stale leftover from a previous failed attempt
        if (rename(s_active.tmp_path, s_active.final_path) != 0) {
            LOG_ERROR("offline: couldn't move completed download into place at '%s'", s_active.final_path);
            remove(s_active.tmp_path);
            ok = false;
        }
    } else {
        // Resumable and still under the retry cap: keep the partial file
        // (and its resume record) and put it back at the front of the
        // queue instead of losing the progress already made - a dropped
        // WiFi connection or a momentary server hiccup shouldn't mean
        // starting a multi-minute FLAC download over from scratch.
        bool resumable = is_ext_playable(s_active.item_ext) && s_active.bytes_done > 0;
        if (resumable && s_active.item.retry_count < OFFLINE_MAX_RETRIES) {
            OfflineQueueItem retry_item = s_active.item;
            retry_item.retry_count++;
            retry_item.retry_not_before = svcGetSystemTick() + retry_backoff_ticks(retry_item.retry_count);
            queue_push_front(&retry_item);
            save_queue();
            LOG_WARN("offline: '%s' failed (http=%ld) - will retry (%d/%d), kept %d bytes",
                     s_active.item.title, http_code, retry_item.retry_count, OFFLINE_MAX_RETRIES, (int)s_active.bytes_done);
        } else {
            remove(s_active.tmp_path);
            clear_resume_record();
            LOG_ERROR("offline: giving up on '%s' after %d attempt(s) (http=%ld)",
                       s_active.item.title, s_active.item.retry_count + 1, http_code);
        }
    }

    if (ok) {
        bool was_direct = is_ext_playable(s_active.item_ext);
        const char* stored_codec = was_direct ? s_active.item.audio_codec : "mp3";
        int stored_bitrate = was_direct ? s_active.item.bitrate : 320;
        add_track_manifest_entry(&s_active.item, s_active.item_ext, stored_codec, stored_bitrate,
                                  (long)s_active.bytes_done);
        request_album_thumb_if_needed(&s_active.item);
        clear_resume_record();
        save_manifest();
        LOG_INFO("offline: finished '%s' (%d bytes)", s_active.item.title, (int)s_active.bytes_done);
    }

    memset(&s_active, 0, sizeof(s_active));
}

// --- Background download thread ----------------------------------------------

// One iteration's worth of work, called with the lock held. Kept separate
// from dl_thread_func() just so the "what to do this iteration" logic reads
// linearly without the lock-acquire boilerplate repeated at each step.
static void dl_thread_tick_locked(void) {
    if (s_cancel_active_requested) {
        if (s_active.valid) cancel_active_download(true);
        s_cancel_active_requested = false;
    }
    if (!s_active.valid && s_queue_count > 0) {
        start_next_download();
    }
}

static void dl_thread_func(void* arg) {
    (void)arg;
    for (;;) {
        LightLock_Lock(&s_lock);
        bool run = s_dl_thread_should_run;
        dl_thread_tick_locked();
        CURLM* multi = s_dl_multi;
        bool active = s_active.valid;
        LightLock_Unlock(&s_lock);

        if (!run) break;

        if (active && multi) {
            // The actual (bounded) blocking wait for socket activity - kept
            // outside the lock so a query from the main thread (e.g. the
            // Downloads screen reading progress) never waits on network I/O.
            int numfds = 0;
            curl_multi_wait(multi, NULL, 0, 200, &numfds);

            LightLock_Lock(&s_lock);
            if (s_active.valid) { // could have been cancelled while the wait above was unlocked
                curl_off_t cap = s_network_streaming_hint ? OFFLINE_DL_THROTTLED_BYTES_PER_SEC : 0;
                curl_easy_setopt(s_active.easy, CURLOPT_MAX_RECV_SPEED_LARGE, cap);

                int running = 0;
                curl_multi_perform(s_dl_multi, &running);

                if (s_active.bytes_total <= 0 && s_active.easy) {
                    curl_off_t cl = 0;
                    if (curl_easy_getinfo(s_active.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK && cl > 0) {
                        s_active.bytes_total = cl;
                    }
                }
                if (running == 0) finish_active_download();
            }
            LightLock_Unlock(&s_lock);
        } else {
            // Nothing to do this round (empty queue, or the front item's
            // still backing off after a failure) - a short idle poll rather
            // than spinning.
            svcSleepThread(200 * 1000 * 1000LL);
        }
    }

    // Shutting down (offline_cleanup() asked us to stop and is waiting via
    // threadJoin()) - pause rather than cancel, so a resumable in-progress
    // download survives to the next launch instead of restarting.
    LightLock_Lock(&s_lock);
    if (s_active.valid) {
        bool keep_partial = is_ext_playable(s_active.item_ext);
        cancel_active_download(!keep_partial);
    }
    if (s_dl_multi) {
        curl_multi_cleanup(s_dl_multi);
        s_dl_multi = NULL;
    }
    LightLock_Unlock(&s_lock);
}

void offline_init(void) {
    LightLock_Init(&s_lock);
    ensure_dirs();
    s_num_tracks = 0;
    s_num_playlists = 0;
    s_queue_count = 0;
    memset(&s_active, 0, sizeof(s_active));
    s_cancel_active_requested = false;
    s_network_streaming_hint = false;
    s_pending_thumb_request = false;

    load_manifest();

    // Restore whatever was still queued (not yet started) from a previous
    // session - see OFFLINE_QUEUE_PATH.
    load_queue();

    // Pick up an interrupted download from a previous run, if there's a
    // valid resume record and it's not already something we'd otherwise
    // re-queue (already finished, or somehow already pending - e.g.
    // load_queue() just restored it too, since it was both still queued
    // *and* the one actively in progress when the app last closed).
    OfflineQueueItem resume_item;
    if (load_resume_record(&resume_item)) {
        if (find_track_index(resume_item.rating_key) < 0 && queue_find(resume_item.rating_key) < 0) {
            queue_push_front(&resume_item);
            LOG_INFO("offline: will resume interrupted download of '%s'", resume_item.title);
        } else {
            clear_resume_record();
        }
    }

    LOG_INFO("offline: loaded %d downloaded track(s), %d playlist(s) (%u bytes on disk), %d queued",
             s_num_tracks, s_num_playlists, (unsigned)offline_get_storage_used_bytes(), s_queue_count);

    s_dl_thread_should_run = true;

    // Give the download thread a lower priority than the main thread so it
    // never preempts rendering/audio work when the two do end up sharing a
    // core (always true on Old3DS; true on New3DS too if core 2 isn't
    // available - see below). Higher numeric value = lower priority.
    s32 main_prio = 0x30; // libctru's typical main-thread default - used as a fallback if the query itself fails
    svcGetThreadPriority(&main_prio, CUR_THREAD_HANDLE);
    int dl_prio = main_prio + 1;
    if (dl_prio > 0x3F) dl_prio = 0x3F;

    bool is_n3ds = false;
    APT_CheckNew3DS(&is_n3ds);
    if (is_n3ds) {
        // Core 2 is New3DS-exclusive and needs an exheader capability bit
        // this app may or may not have (see thread.h's threadCreate() doc
        // comment) - if it's not granted, this just returns NULL and the
        // fallback below still gets a real background thread, just sharing
        // a core with everything else instead of running on a free one.
        s_dl_thread = threadCreate(dl_thread_func, NULL, OFFLINE_DL_THREAD_STACK_SIZE, dl_prio, 2, false);
    }
    if (!s_dl_thread) {
        s_dl_thread = threadCreate(dl_thread_func, NULL, OFFLINE_DL_THREAD_STACK_SIZE, dl_prio, -2, false);
    }
    if (!s_dl_thread) {
        LOG_ERROR("offline: failed to create the background download thread - downloads are disabled this session");
        s_dl_thread_should_run = false;
    } else {
        LOG_INFO("offline: download thread started (%s)", is_n3ds ? "New3DS" : "Old3DS");
    }
}

void offline_set_network_streaming_hint(bool is_streaming) {
    s_network_streaming_hint = is_streaming;
}

void offline_update(void) {
    // The only main-thread-side follow-up work left after moving the actual
    // transfer to the background thread - see request_album_thumb_if_needed().
    bool have_request = false;
    char url[PLEX_MAX_URL] = "";
    char path[512] = "";

    LightLock_Lock(&s_lock);
    if (s_pending_thumb_request) {
        strncpy(url, s_pending_thumb_url, sizeof(url) - 1);
        strncpy(path, s_pending_thumb_path, sizeof(path) - 1);
        s_pending_thumb_request = false;
        have_request = true;
    }
    LightLock_Unlock(&s_lock);

    if (have_request) {
        u8* data = NULL;
        size_t size = 0;
        if (plex_api_get_album_art(url, &data, &size) && data && size > 0) {
            FILE* f = fopen(path, "wb");
            if (f) {
                fwrite(data, 1, size, f);
                fclose(f);
            }
        }
        if (data) free(data);
    }
}

void offline_cleanup(void) {
    LightLock_Lock(&s_lock);
    s_dl_thread_should_run = false;
    LightLock_Unlock(&s_lock);

    if (s_dl_thread) {
        threadJoin(s_dl_thread, U64_MAX);
        threadFree(s_dl_thread);
        s_dl_thread = NULL;
    }

    // The thread's fully stopped now - safe to free without the lock.
    free(s_queue);
    s_queue = NULL;
    s_queue_capacity = 0;
}

void offline_get_download_status(OfflineDownloadStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    LightLock_Lock(&s_lock);
    out->queue_remaining = s_queue_count;
    if (s_active.valid) {
        out->active = true;
        strncpy(out->title, s_active.item.title, sizeof(out->title) - 1);
        strncpy(out->artist_title, s_active.item.artist_title, sizeof(out->artist_title) - 1);
        out->bytes_done = s_active.bytes_done;
        out->bytes_total = s_active.bytes_total > 0 ? (size_t)s_active.bytes_total : 0;
    }
    LightLock_Unlock(&s_lock);
}

// --- Querying what's on disk -------------------------------------------------

bool offline_track_is_downloaded(const char* rating_key) {
    LightLock_Lock(&s_lock);
    bool result = find_track_index(rating_key) >= 0;
    LightLock_Unlock(&s_lock);
    return result;
}

bool offline_artist_has_any_downloaded(const char* artist_rating_key) {
    if (!artist_rating_key || !artist_rating_key[0]) return false;
    LightLock_Lock(&s_lock);
    bool found = false;
    for (int i = 0; i < s_num_tracks; i++) {
        if (strcmp(artist_identity(&s_tracks[i]), artist_rating_key) == 0) { found = true; break; }
    }
    LightLock_Unlock(&s_lock);
    return found;
}

bool offline_playlist_has_any_downloaded(const char* playlist_rating_key) {
    LightLock_Lock(&s_lock);
    bool found = false;
    int pi = find_playlist_index(playlist_rating_key);
    if (pi >= 0) {
        OfflinePlaylistEntry* p = &s_playlists[pi];
        for (int i = 0; i < p->track_count; i++) {
            if (find_track_index(p->track_keys[i]) >= 0) { found = true; break; }
        }
    }
    LightLock_Unlock(&s_lock);
    return found;
}

bool offline_get_track_playback_url(const char* rating_key, char* url_out, size_t url_max) {
    LightLock_Lock(&s_lock);
    int ti = find_track_index(rating_key);
    bool ok = ti >= 0;
    if (ok) {
        char path[512];
        track_file_path(s_tracks[ti].rating_key, s_tracks[ti].ext, path, sizeof(path));
        snprintf(url_out, url_max, "file://%s", path);
    }
    LightLock_Unlock(&s_lock);
    return ok;
}

bool offline_get_thumb_path(const char* rating_key, char* path_out, size_t path_max) {
    LightLock_Lock(&s_lock);
    int ti = find_track_index(rating_key);
    bool ok = false;
    if (ti >= 0) {
        const char* aid = album_identity(&s_tracks[ti]);
        if (aid[0]) {
            char path[512];
            album_thumb_path(aid, path, sizeof(path));
            FILE* f = fopen(path, "rb");
            if (f) {
                fclose(f);
                strncpy(path_out, path, path_max - 1);
                path_out[path_max - 1] = '\0';
                ok = true;
            }
        }
    }
    LightLock_Unlock(&s_lock);
    return ok;
}

size_t offline_get_storage_used_bytes(void) {
    LightLock_Lock(&s_lock);
    size_t total = 0;
    for (int i = 0; i < s_num_tracks; i++) {
        if (s_tracks[i].file_size > 0) total += (size_t)s_tracks[i].file_size;
    }
    LightLock_Unlock(&s_lock);
    return total;
}

int offline_get_track_count(void) {
    LightLock_Lock(&s_lock);
    int n = s_num_tracks;
    LightLock_Unlock(&s_lock);
    return n;
}
int offline_get_playlist_count(void) {
    LightLock_Lock(&s_lock);
    int n = s_num_playlists;
    LightLock_Unlock(&s_lock);
    return n;
}

int offline_get_artist_count(void) {
    LightLock_Lock(&s_lock);
    const char* seen[OFFLINE_MAX_TRACKS];
    int n = 0;
    for (int i = 0; i < s_num_tracks; i++) {
        const char* id = artist_identity(&s_tracks[i]);
        if (!id[0]) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(seen[j], id) == 0) { dup = true; break; }
        }
        if (!dup) seen[n++] = id;
    }
    LightLock_Unlock(&s_lock);
    return n;
}

int offline_get_album_count(void) {
    LightLock_Lock(&s_lock);
    const char* seen[OFFLINE_MAX_TRACKS];
    int n = 0;
    for (int i = 0; i < s_num_tracks; i++) {
        const char* id = album_identity(&s_tracks[i]);
        if (!id[0]) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(seen[j], id) == 0) { dup = true; break; }
        }
        if (!dup) seen[n++] = id;
    }
    LightLock_Unlock(&s_lock);
    return n;
}

// --- Browsing -----------------------------------------------------------

static int cmp_artist_title(const void* a, const void* b) {
    return strcasecmp(((const PlexArtist*)a)->title, ((const PlexArtist*)b)->title);
}
static int cmp_album_title(const void* a, const void* b) {
    return strcasecmp(((const PlexAlbum*)a)->title, ((const PlexAlbum*)b)->title);
}
static int cmp_playlist_title(const void* a, const void* b) {
    return strcasecmp(((const PlexPlaylist*)a)->title, ((const PlexPlaylist*)b)->title);
}
static int cmp_track_index(const void* a, const void* b) {
    return ((const PlexTrack*)a)->index - ((const PlexTrack*)b)->index;
}

int offline_get_artists(PlexArtist* out, int max) {
    if (!out || max <= 0) return 0;
    LightLock_Lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_num_tracks && n < max; i++) {
        const char* id = artist_identity(&s_tracks[i]);
        if (!id[0]) continue;
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].rating_key, id) == 0) { dup = true; break; }
        }
        if (dup) continue;

        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].rating_key, id, sizeof(out[n].rating_key) - 1);
        strncpy(out[n].key, id, sizeof(out[n].key) - 1);
        const char* title = s_tracks[i].artist_title[0] ? s_tracks[i].artist_title : "Unknown Artist";
        strncpy(out[n].title, title, sizeof(out[n].title) - 1);
        n++;
    }
    LightLock_Unlock(&s_lock);
    qsort(out, n, sizeof(PlexArtist), cmp_artist_title);
    return n;
}

int offline_get_albums(const char* artist_rating_key, PlexAlbum* out, int max) {
    if (!out || max <= 0 || !artist_rating_key) return 0;
    LightLock_Lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_num_tracks && n < max; i++) {
        OfflineTrackEntry* e = &s_tracks[i];
        if (strcmp(artist_identity(e), artist_rating_key) != 0) continue;
        const char* aid = album_identity(e);
        if (!aid[0]) continue;

        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (strcmp(out[j].rating_key, aid) == 0) { dup = true; break; }
        }
        if (dup) continue;

        memset(&out[n], 0, sizeof(out[n]));
        strncpy(out[n].rating_key, aid, sizeof(out[n].rating_key) - 1);
        strncpy(out[n].key, aid, sizeof(out[n].key) - 1);
        const char* title = e->album_title[0] ? e->album_title : "Unknown Album";
        strncpy(out[n].title, title, sizeof(out[n].title) - 1);
        strncpy(out[n].parent_title, e->artist_title, sizeof(out[n].parent_title) - 1);
        n++;
    }
    for (int i = 0; i < n; i++) {
        int count = 0;
        for (int j = 0; j < s_num_tracks; j++) {
            if (strcmp(album_identity(&s_tracks[j]), out[i].rating_key) == 0) count++;
        }
        out[i].leaf_count = count;
    }
    LightLock_Unlock(&s_lock);
    qsort(out, n, sizeof(PlexAlbum), cmp_album_title);
    return n;
}

int offline_get_tracks_for_album(const char* album_rating_key, PlexTrack* out, int max) {
    if (!out || max <= 0 || !album_rating_key) return 0;
    LightLock_Lock(&s_lock);
    int n = 0;
    for (int i = 0; i < s_num_tracks && n < max; i++) {
        OfflineTrackEntry* e = &s_tracks[i];
        if (strcmp(album_identity(e), album_rating_key) != 0) continue;
        fill_plex_track_from_entry(e, &out[n]);
        n++;
    }
    LightLock_Unlock(&s_lock);
    qsort(out, n, sizeof(PlexTrack), cmp_track_index);
    return n;
}

int offline_get_playlists(PlexPlaylist* out, int max) {
    if (!out || max <= 0) return 0;
    LightLock_Lock(&s_lock);
    int n = s_num_playlists < max ? s_num_playlists : max;
    for (int i = 0; i < n; i++) {
        OfflinePlaylistEntry* p = &s_playlists[i];
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].rating_key, p->rating_key, sizeof(out[i].rating_key) - 1);
        strncpy(out[i].key, p->rating_key, sizeof(out[i].key) - 1);
        strncpy(out[i].title, p->title, sizeof(out[i].title) - 1);
        out[i].leaf_count = p->track_count;

        int duration = 0;
        for (int j = 0; j < p->track_count; j++) {
            int ti = find_track_index(p->track_keys[j]);
            if (ti >= 0) duration += s_tracks[ti].duration;
        }
        out[i].duration = duration;
    }
    LightLock_Unlock(&s_lock);
    qsort(out, n, sizeof(PlexPlaylist), cmp_playlist_title);
    return n;
}

int offline_get_tracks_for_playlist(const char* playlist_rating_key, PlexTrack* out, int max) {
    if (!out || max <= 0) return 0;
    LightLock_Lock(&s_lock);
    int n = 0;
    int pi = find_playlist_index(playlist_rating_key);
    if (pi >= 0) {
        OfflinePlaylistEntry* p = &s_playlists[pi];
        for (int i = 0; i < p->track_count && n < max; i++) {
            int ti = find_track_index(p->track_keys[i]);
            if (ti < 0) continue; // deleted independently since this playlist was downloaded - just skip it
            fill_plex_track_from_entry(&s_tracks[ti], &out[n]);
            n++;
        }
    }
    LightLock_Unlock(&s_lock);
    return n;
}

// --- Deleting -----------------------------------------------------------

// Drops one reference on `rating_key`, actually deleting its file (and its
// album's cached thumbnail, if nothing downloaded still needs it) once the
// count reaches zero. Returns true if the entry was actually removed from
// the manifest (false if it's untouched - not found at all, or still kept
// alive by another reference) - callers iterating s_tracks[] need to know
// this, since a removal reshuffles the array (see its callers below) but a
// mere ref-count decrement leaves the entry sitting right where it was.
static bool release_track_ref(const char* rating_key) {
    int ti = find_track_index(rating_key);
    if (ti < 0) return false;

    OfflineTrackEntry* e = &s_tracks[ti];
    e->ref_count--;
    if (e->ref_count > 0) return false;

    char path[512];
    track_file_path(e->rating_key, e->ext, path, sizeof(path));
    remove(path);

    char album_id[PLEX_MAX_STR];
    strncpy(album_id, album_identity(e), sizeof(album_id) - 1);
    album_id[sizeof(album_id) - 1] = '\0';

    // Remove the entry (order doesn't matter to any caller - swap with last).
    int last = s_num_tracks - 1;
    if (ti != last) s_tracks[ti] = s_tracks[last];
    s_num_tracks--;

    if (album_id[0]) {
        bool still_referenced = false;
        for (int i = 0; i < s_num_tracks; i++) {
            if (strcmp(album_identity(&s_tracks[i]), album_id) == 0) { still_referenced = true; break; }
        }
        if (!still_referenced) {
            char thumb_path[512];
            album_thumb_path(album_id, thumb_path, sizeof(thumb_path));
            remove(thumb_path);
        }
    }
    return true;
}

static bool queue_item_matches_rating_key(const OfflineQueueItem* q, const char* id) {
    return strcmp(q->rating_key, id) == 0;
}
static bool queue_item_matches_album(const OfflineQueueItem* q, const char* id) {
    const char* qid = q->album_rating_key[0] ? q->album_rating_key : q->album_title;
    return qid[0] && strcmp(qid, id) == 0;
}
static bool queue_item_matches_artist(const OfflineQueueItem* q, const char* id) {
    const char* qid = q->artist_rating_key[0] ? q->artist_rating_key : q->artist_title;
    return qid[0] && strcmp(qid, id) == 0;
}

// Drops any not-yet-downloaded queue entries matching `id` per `match`, and
// requests cancellation of the in-flight transfer if it's one of them (see
// the module comment on why this is a request the download thread itself
// carries out, rather than this function - running on the main thread -
// touching curl objects directly) - so deleting an album/artist/track also
// stops it from reappearing a moment later once its still-pending download
// completes.
static void purge_queue_and_active(bool (*match)(const OfflineQueueItem*, const char*), const char* id) {
    int removed = 0;
    int i = 0;
    while (i < s_queue_count) {
        if (match(&s_queue[i], id)) {
            memmove(&s_queue[i], &s_queue[i + 1], (size_t)(s_queue_count - 1 - i) * sizeof(OfflineQueueItem));
            s_queue_count--;
            removed++;
        } else {
            i++;
        }
    }
    if (removed > 0) save_queue(); // keep OFFLINE_QUEUE_PATH from resurrecting deleted items on next launch

    if (s_active.valid && match(&s_active.item, id)) {
        s_cancel_active_requested = true;
    }
}

void offline_delete_track(const char* rating_key) {
    if (!rating_key || !rating_key[0]) return;
    LightLock_Lock(&s_lock);
    purge_queue_and_active(queue_item_matches_rating_key, rating_key);
    release_track_ref(rating_key);
    save_manifest();
    LightLock_Unlock(&s_lock);
}

void offline_delete_album(const char* album_rating_key) {
    if (!album_rating_key || !album_rating_key[0]) return;
    LightLock_Lock(&s_lock);
    purge_queue_and_active(queue_item_matches_album, album_rating_key);

    // release_track_ref() only removes the matched entry (via swap-with-last,
    // which moves an unvisited entry into the slot we're currently looking
    // at) if this was its last reference - if something else still needs it
    // (e.g. also part of a downloaded playlist), it's left in place with
    // just its ref_count decremented. Only skip advancing `i` in the former
    // case; the latter would otherwise re-match the very same untouched
    // entry forever.
    int i = 0;
    while (i < s_num_tracks) {
        if (strcmp(album_identity(&s_tracks[i]), album_rating_key) == 0) {
            char rk[PLEX_MAX_STR];
            strncpy(rk, s_tracks[i].rating_key, sizeof(rk) - 1);
            rk[sizeof(rk) - 1] = '\0';
            if (!release_track_ref(rk)) i++;
        } else {
            i++;
        }
    }
    save_manifest();
    LightLock_Unlock(&s_lock);
}

void offline_delete_artist(const char* artist_rating_key) {
    if (!artist_rating_key || !artist_rating_key[0]) return;
    LightLock_Lock(&s_lock);
    purge_queue_and_active(queue_item_matches_artist, artist_rating_key);

    // See offline_delete_album()'s comment on why `i` only advances when
    // release_track_ref() reports the entry wasn't actually removed.
    int i = 0;
    while (i < s_num_tracks) {
        if (strcmp(artist_identity(&s_tracks[i]), artist_rating_key) == 0) {
            char rk[PLEX_MAX_STR];
            strncpy(rk, s_tracks[i].rating_key, sizeof(rk) - 1);
            rk[sizeof(rk) - 1] = '\0';
            if (!release_track_ref(rk)) i++;
        } else {
            i++;
        }
    }
    save_manifest();
    LightLock_Unlock(&s_lock);
}

void offline_delete_playlist(const char* playlist_rating_key) {
    LightLock_Lock(&s_lock);
    int pi = find_playlist_index(playlist_rating_key);
    if (pi >= 0) {
        OfflinePlaylistEntry p = s_playlists[pi]; // copy - the array below gets reshuffled
        int last = s_num_playlists - 1;
        if (pi != last) s_playlists[pi] = s_playlists[last];
        s_num_playlists--;

        for (int i = 0; i < p.track_count; i++) {
            release_track_ref(p.track_keys[i]);
        }
        save_manifest();
    }
    LightLock_Unlock(&s_lock);
}

void offline_delete_all(void) {
    LightLock_Lock(&s_lock);
    s_queue_count = 0;
    bool need_cancel = s_active.valid;
    if (need_cancel) s_cancel_active_requested = true;
    LightLock_Unlock(&s_lock);

    if (need_cancel) {
        // Bounded wait for the download thread to actually release the
        // active transfer's file/curl handles before wiping the tracks/
        // directory below out from under it - see purge_queue_and_active()'s
        // comment on why a single-item delete doesn't need this (no such
        // race there - a track that's still downloading was never added to
        // the manifest, so there's no completed file for it to collide
        // with), but a full directory wipe does.
        for (int waited_ms = 0; waited_ms < 1000; waited_ms += 20) {
            LightLock_Lock(&s_lock);
            bool still_active = s_active.valid;
            LightLock_Unlock(&s_lock);
            if (!still_active) break;
            svcSleepThread(20 * 1000 * 1000LL);
        }
    }

    LightLock_Lock(&s_lock);
    remove_all_files_in_dir(OFFLINE_TRACKS_DIR);
    remove_all_files_in_dir(OFFLINE_THUMBS_DIR);
    clear_resume_record();
    save_queue(); // s_queue_count was already zeroed above - persist the now-empty queue
    s_num_tracks = 0;
    s_num_playlists = 0;
    save_manifest();
    LOG_INFO("offline: deleted all downloads");
    LightLock_Unlock(&s_lock);
}
