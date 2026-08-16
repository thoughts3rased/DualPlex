// Offline downloads: a persistent SD-card library (library.json + tracks/ +
// thumbs/) of tracks the user has explicitly chosen to save, plus a small
// one-at-a-time background download engine that fills it in. See offline.h
// for the public shape; this file also owns the download queue and the
// manifest's on-disk JSON format.
#include "offline.h"
#include "plex_api.h"
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
// trip" cache, not a full mirror of a Plex server.
#define OFFLINE_MAX_TRACKS 500
#define OFFLINE_MAX_PLAYLISTS 30
#define OFFLINE_PLAYLIST_MAX_TRACKS 100
#define OFFLINE_MAX_QUEUE 150

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

// A download not yet started - metadata copied out of the PlexTrack that was
// queued (which the caller's own array may go on to overwrite/reuse before
// this actually gets downloaded, e.g. navigating to a different album).
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

static OfflineTrackEntry s_tracks[OFFLINE_MAX_TRACKS];
static int s_num_tracks = 0;
static OfflinePlaylistEntry s_playlists[OFFLINE_MAX_PLAYLISTS];
static int s_num_playlists = 0;

static OfflineQueueItem s_queue[OFFLINE_MAX_QUEUE];
static int s_queue_count = 0;

static CURLM* s_dl_multi = NULL;
static ActiveDownload s_active = {0};

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

void offline_init(void) {
    ensure_dirs();
    s_num_tracks = 0;
    s_num_playlists = 0;
    s_queue_count = 0;
    memset(&s_active, 0, sizeof(s_active));
    load_manifest();
    LOG_INFO("offline: loaded %d downloaded track(s), %d playlist(s) (%u bytes on disk)",
             s_num_tracks, s_num_playlists, (unsigned)offline_get_storage_used_bytes());
}

// --- Queueing ----------------------------------------------------------------

static void queue_push(const PlexTrack* t) {
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

int offline_queue_tracks(const PlexTrack* tracks, int count) {
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

        if (s_queue_count >= OFFLINE_MAX_QUEUE) {
            LOG_WARN("offline: download queue full (%d) - dropping '%s'", OFFLINE_MAX_QUEUE, t->title);
            continue;
        }
        queue_push(t);
        newly_queued++;
    }

    if (dirty) save_manifest();
    return newly_queued;
}

int offline_queue_playlist(const char* playlist_rating_key, const char* playlist_title,
                            const PlexTrack* tracks, int count) {
    if (!playlist_rating_key || !playlist_rating_key[0]) return 0;
    int newly = offline_queue_tracks(tracks, count);

    int pi = find_playlist_index(playlist_rating_key);
    if (pi < 0) {
        if (s_num_playlists >= OFFLINE_MAX_PLAYLISTS) {
            LOG_WARN("offline: playlist manifest full (%d) - can't add '%s'",
                     OFFLINE_MAX_PLAYLISTS, playlist_title ? playlist_title : "?");
            return newly;
        }
        pi = s_num_playlists++;
    }

    OfflinePlaylistEntry* p = &s_playlists[pi];
    memset(p, 0, sizeof(*p));
    strncpy(p->rating_key, playlist_rating_key, sizeof(p->rating_key) - 1);
    strncpy(p->title, playlist_title ? playlist_title : "", sizeof(p->title) - 1);
    int n = count < OFFLINE_PLAYLIST_MAX_TRACKS ? count : OFFLINE_PLAYLIST_MAX_TRACKS;
    for (int i = 0; i < n; i++) {
        if (!tracks[i].rating_key[0]) continue;
        strncpy(p->track_keys[p->track_count], tracks[i].rating_key, sizeof(p->track_keys[0]) - 1);
        p->track_count++;
    }

    save_manifest();
    return newly;
}

// --- Download engine -----------------------------------------------------

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
// start_next_download() - so it's guaranteed playable offline.
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

// Caches one thumbnail per album (not per track - every track on the same
// album shares the same cover art) under OFFLINE_THUMBS_DIR, for
// offline_get_thumb_path()/album_art_load_local() to show art on the Now
// Playing screen with no server connection. Blocking (reuses
// plex_api_get_album_art(), which pumps audio_player_update() internally
// while it waits - same as every other blocking plex_api_* call), but only
// runs once per album's first downloaded track, not once per track.
static void maybe_fetch_album_thumb(const OfflineQueueItem* item) {
    if (!item->thumb[0]) return;
    const char* album_id = item->album_rating_key[0] ? item->album_rating_key
                          : (item->album_title[0] ? item->album_title : item->rating_key);

    char path[512];
    album_thumb_path(album_id, path, sizeof(path));

    FILE* existing = fopen(path, "rb");
    if (existing) {
        fclose(existing);
        return;
    }

    u8* data = NULL;
    size_t size = 0;
    if (plex_api_get_album_art(item->thumb, &data, &size) && data && size > 0) {
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(data, 1, size, f);
            fclose(f);
        }
    }
    if (data) free(data);
}

static void cancel_active_download(void) {
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
    remove(s_active.tmp_path);
    memset(&s_active, 0, sizeof(s_active));
}

static void start_next_download(void) {
    if (s_queue_count <= 0) return;

    OfflineQueueItem item = s_queue[0];
    memmove(&s_queue[0], &s_queue[1], (size_t)(s_queue_count - 1) * sizeof(OfflineQueueItem));
    s_queue_count--;

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

    s_active.fp = fopen(s_active.tmp_path, "wb");
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

    if (!s_dl_multi) s_dl_multi = curl_multi_init();
    curl_multi_add_handle(s_dl_multi, s_active.easy);

    s_active.item = item;
    strncpy(s_active.item_ext, ext, sizeof(s_active.item_ext) - 1);
    s_active.bytes_done = 0;
    s_active.bytes_total = 0;
    s_active.valid = true;

    LOG_INFO("offline: downloading '%s' -> %s", item.title, s_active.final_path);
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
    ok = ok && http_code == 200 && s_active.bytes_done > 0;

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
        LOG_ERROR("offline: download failed for '%s' (http=%ld)", s_active.item.title, http_code);
        remove(s_active.tmp_path);
    }

    if (ok) {
        bool was_direct = is_ext_playable(s_active.item_ext);
        const char* stored_codec = was_direct ? s_active.item.audio_codec : "mp3";
        int stored_bitrate = was_direct ? s_active.item.bitrate : 320;
        add_track_manifest_entry(&s_active.item, s_active.item_ext, stored_codec, stored_bitrate,
                                  (long)s_active.bytes_done);
        maybe_fetch_album_thumb(&s_active.item);
        save_manifest();
        LOG_INFO("offline: finished '%s' (%d bytes)", s_active.item.title, (int)s_active.bytes_done);
    }

    memset(&s_active, 0, sizeof(s_active));
}

void offline_cleanup(void) {
    if (s_active.valid) cancel_active_download();
    if (s_dl_multi) {
        curl_multi_cleanup(s_dl_multi);
        s_dl_multi = NULL;
    }
}

void offline_update(void) {
    if (s_active.valid) {
        int running = 0;
        curl_multi_perform(s_dl_multi, &running);

        if (s_active.bytes_total <= 0 && s_active.easy) {
            curl_off_t cl = 0;
            if (curl_easy_getinfo(s_active.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl) == CURLE_OK && cl > 0) {
                s_active.bytes_total = cl;
            }
        }

        if (running == 0) finish_active_download();
        return;
    }

    if (s_queue_count > 0) start_next_download();
}

void offline_get_download_status(OfflineDownloadStatus* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->queue_remaining = s_queue_count;
    if (!s_active.valid) return;

    out->active = true;
    strncpy(out->title, s_active.item.title, sizeof(out->title) - 1);
    strncpy(out->artist_title, s_active.item.artist_title, sizeof(out->artist_title) - 1);
    out->bytes_done = s_active.bytes_done;
    out->bytes_total = s_active.bytes_total > 0 ? (size_t)s_active.bytes_total : 0;
}

// --- Querying what's on disk -------------------------------------------------

bool offline_track_is_downloaded(const char* rating_key) {
    return find_track_index(rating_key) >= 0;
}

bool offline_artist_has_any_downloaded(const char* artist_rating_key) {
    if (!artist_rating_key || !artist_rating_key[0]) return false;
    for (int i = 0; i < s_num_tracks; i++) {
        if (strcmp(artist_identity(&s_tracks[i]), artist_rating_key) == 0) return true;
    }
    return false;
}

bool offline_playlist_has_any_downloaded(const char* playlist_rating_key) {
    int pi = find_playlist_index(playlist_rating_key);
    if (pi < 0) return false;
    OfflinePlaylistEntry* p = &s_playlists[pi];
    for (int i = 0; i < p->track_count; i++) {
        if (find_track_index(p->track_keys[i]) >= 0) return true;
    }
    return false;
}

bool offline_get_track_playback_url(const char* rating_key, char* url_out, size_t url_max) {
    int ti = find_track_index(rating_key);
    if (ti < 0) return false;
    char path[512];
    track_file_path(s_tracks[ti].rating_key, s_tracks[ti].ext, path, sizeof(path));
    snprintf(url_out, url_max, "file://%s", path);
    return true;
}

bool offline_get_thumb_path(const char* rating_key, char* path_out, size_t path_max) {
    int ti = find_track_index(rating_key);
    if (ti < 0) return false;
    const char* aid = album_identity(&s_tracks[ti]);
    if (!aid[0]) return false;

    char path[512];
    album_thumb_path(aid, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);

    strncpy(path_out, path, path_max - 1);
    path_out[path_max - 1] = '\0';
    return true;
}

size_t offline_get_storage_used_bytes(void) {
    size_t total = 0;
    for (int i = 0; i < s_num_tracks; i++) {
        if (s_tracks[i].file_size > 0) total += (size_t)s_tracks[i].file_size;
    }
    return total;
}

int offline_get_track_count(void) { return s_num_tracks; }
int offline_get_playlist_count(void) { return s_num_playlists; }

int offline_get_artist_count(void) {
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
    return n;
}

int offline_get_album_count(void) {
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
    qsort(out, n, sizeof(PlexArtist), cmp_artist_title);
    return n;
}

int offline_get_albums(const char* artist_rating_key, PlexAlbum* out, int max) {
    if (!out || max <= 0 || !artist_rating_key) return 0;
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
    qsort(out, n, sizeof(PlexAlbum), cmp_album_title);
    return n;
}

int offline_get_tracks_for_album(const char* album_rating_key, PlexTrack* out, int max) {
    if (!out || max <= 0 || !album_rating_key) return 0;
    int n = 0;
    for (int i = 0; i < s_num_tracks && n < max; i++) {
        OfflineTrackEntry* e = &s_tracks[i];
        if (strcmp(album_identity(e), album_rating_key) != 0) continue;
        fill_plex_track_from_entry(e, &out[n]);
        n++;
    }
    qsort(out, n, sizeof(PlexTrack), cmp_track_index);
    return n;
}

int offline_get_playlists(PlexPlaylist* out, int max) {
    if (!out || max <= 0) return 0;
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
    qsort(out, n, sizeof(PlexPlaylist), cmp_playlist_title);
    return n;
}

int offline_get_tracks_for_playlist(const char* playlist_rating_key, PlexTrack* out, int max) {
    if (!out || max <= 0) return 0;
    int pi = find_playlist_index(playlist_rating_key);
    if (pi < 0) return 0;

    OfflinePlaylistEntry* p = &s_playlists[pi];
    int n = 0;
    for (int i = 0; i < p->track_count && n < max; i++) {
        int ti = find_track_index(p->track_keys[i]);
        if (ti < 0) continue; // deleted independently since this playlist was downloaded - just skip it
        fill_plex_track_from_entry(&s_tracks[ti], &out[n]);
        n++;
    }
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

// Drops any not-yet-downloaded queue entries (and cancels the in-flight
// transfer, if it's one of them) matching `id` per `match` - so deleting an
// album/artist/track also stops it from reappearing a moment later once its
// still-pending download completes.
static void purge_queue_and_active(bool (*match)(const OfflineQueueItem*, const char*), const char* id) {
    int i = 0;
    while (i < s_queue_count) {
        if (match(&s_queue[i], id)) {
            memmove(&s_queue[i], &s_queue[i + 1], (size_t)(s_queue_count - 1 - i) * sizeof(OfflineQueueItem));
            s_queue_count--;
        } else {
            i++;
        }
    }
    if (s_active.valid && match(&s_active.item, id)) {
        cancel_active_download();
    }
}

void offline_delete_track(const char* rating_key) {
    if (!rating_key || !rating_key[0]) return;
    purge_queue_and_active(queue_item_matches_rating_key, rating_key);
    release_track_ref(rating_key);
    save_manifest();
}

void offline_delete_album(const char* album_rating_key) {
    if (!album_rating_key || !album_rating_key[0]) return;
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
}

void offline_delete_artist(const char* artist_rating_key) {
    if (!artist_rating_key || !artist_rating_key[0]) return;
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
}

void offline_delete_playlist(const char* playlist_rating_key) {
    int pi = find_playlist_index(playlist_rating_key);
    if (pi < 0) return;

    OfflinePlaylistEntry p = s_playlists[pi]; // copy - the array below gets reshuffled
    int last = s_num_playlists - 1;
    if (pi != last) s_playlists[pi] = s_playlists[last];
    s_num_playlists--;

    for (int i = 0; i < p.track_count; i++) {
        release_track_ref(p.track_keys[i]);
    }
    save_manifest();
}

void offline_delete_all(void) {
    s_queue_count = 0;
    if (s_active.valid) cancel_active_download();

    remove_all_files_in_dir(OFFLINE_TRACKS_DIR);
    remove_all_files_in_dir(OFFLINE_THUMBS_DIR);

    s_num_tracks = 0;
    s_num_playlists = 0;
    save_manifest();
    LOG_INFO("offline: deleted all downloads");
}
