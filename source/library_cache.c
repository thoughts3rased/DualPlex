// See library_cache.h. Small sibling to offline.c's downloaded-content
// manifest: this one just remembers the *shape* of the online library
// (playlist/artist names) so a lost connection doesn't turn those screens
// blank - ui.c falls back to this when a live fetch comes back empty, and
// greys out whatever in it isn't actually downloaded (see offline.c).
#include "library_cache.h"
#include "logger.h"

#include <3ds.h>
#include "lib/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE_MAX_ITEMS 500 // matches PLEX_MAX_ITEMS elsewhere - this app doesn't try to support unbounded libraries anywhere

typedef struct {
    char rating_key[PLEX_MAX_STR];
    char title[PLEX_MAX_STR];
    int leaf_count; // playlists only - always 0 for a cached artist
} CachedEntry;

static CachedEntry s_cached_playlists[CACHE_MAX_ITEMS];
static int s_num_cached_playlists = 0;

static char s_cached_artists_library_key[PLEX_MAX_URL] = "";
static CachedEntry s_cached_artists[CACHE_MAX_ITEMS];
static int s_num_cached_artists = 0;

static void ensure_dir(void) {
    // Shares offline.c's directory - both live under /3ds/dualplex/offline/.
    // A plain single-level mkdir is enough here since main.c's config_save()
    // (which runs before this on every relevant boot path) already created
    // /3ds/dualplex, and offline_init() creates /3ds/dualplex/offline itself
    // - this only needs that immediate parent to already exist.
    mkdir("/3ds/dualplex/offline", S_IRWXU);
}

static void save_cache_file(void) {
    ensure_dir();

    cJSON* root = cJSON_CreateObject();

    cJSON* playlists = cJSON_CreateArray();
    for (int i = 0; i < s_num_cached_playlists; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "rating_key", s_cached_playlists[i].rating_key);
        cJSON_AddStringToObject(o, "title", s_cached_playlists[i].title);
        cJSON_AddNumberToObject(o, "leaf_count", s_cached_playlists[i].leaf_count);
        cJSON_AddItemToArray(playlists, o);
    }
    cJSON_AddItemToObject(root, "playlists", playlists);

    cJSON_AddStringToObject(root, "artists_library_key", s_cached_artists_library_key);
    cJSON* artists = cJSON_CreateArray();
    for (int i = 0; i < s_num_cached_artists; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "rating_key", s_cached_artists[i].rating_key);
        cJSON_AddStringToObject(o, "title", s_cached_artists[i].title);
        cJSON_AddItemToArray(artists, o);
    }
    cJSON_AddItemToObject(root, "artists", artists);

    char* text = cJSON_PrintUnformatted(root);
    if (text) {
        FILE* f = fopen(LIBRARY_CACHE_PATH, "w");
        if (f) {
            fputs(text, f);
            fclose(f);
        } else {
            LOG_ERROR("library_cache: failed to write %s", LIBRARY_CACHE_PATH);
        }
        cJSON_free(text);
    }
    cJSON_Delete(root);
}

void library_cache_init(void) {
    s_num_cached_playlists = 0;
    s_num_cached_artists = 0;
    s_cached_artists_library_key[0] = '\0';

    FILE* f = fopen(LIBRARY_CACHE_PATH, "r");
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
        LOG_WARN("library_cache: %s failed to parse - starting empty", LIBRARY_CACHE_PATH);
        return;
    }

    cJSON* playlists = cJSON_GetObjectItem(root, "playlists");
    if (playlists && cJSON_IsArray(playlists)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, playlists) {
            if (s_num_cached_playlists >= CACHE_MAX_ITEMS) break;
            cJSON* rk = cJSON_GetObjectItem(item, "rating_key");
            cJSON* ti = cJSON_GetObjectItem(item, "title");
            cJSON* lc = cJSON_GetObjectItem(item, "leaf_count");
            if (!rk || !cJSON_IsString(rk) || !rk->valuestring[0]) continue;
            CachedEntry* e = &s_cached_playlists[s_num_cached_playlists++];
            memset(e, 0, sizeof(*e));
            strncpy(e->rating_key, rk->valuestring, sizeof(e->rating_key) - 1);
            if (ti && cJSON_IsString(ti)) strncpy(e->title, ti->valuestring, sizeof(e->title) - 1);
            if (lc && cJSON_IsNumber(lc)) e->leaf_count = lc->valueint;
        }
    }

    cJSON* lib_key = cJSON_GetObjectItem(root, "artists_library_key");
    if (lib_key && cJSON_IsString(lib_key)) {
        strncpy(s_cached_artists_library_key, lib_key->valuestring, sizeof(s_cached_artists_library_key) - 1);
    }
    cJSON* artists = cJSON_GetObjectItem(root, "artists");
    if (artists && cJSON_IsArray(artists)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, artists) {
            if (s_num_cached_artists >= CACHE_MAX_ITEMS) break;
            cJSON* rk = cJSON_GetObjectItem(item, "rating_key");
            cJSON* ti = cJSON_GetObjectItem(item, "title");
            if (!rk || !cJSON_IsString(rk) || !rk->valuestring[0]) continue;
            CachedEntry* e = &s_cached_artists[s_num_cached_artists++];
            memset(e, 0, sizeof(*e));
            strncpy(e->rating_key, rk->valuestring, sizeof(e->rating_key) - 1);
            if (ti && cJSON_IsString(ti)) strncpy(e->title, ti->valuestring, sizeof(e->title) - 1);
        }
    }

    cJSON_Delete(root);
    LOG_INFO("library_cache: loaded %d cached playlist(s), %d cached artist(s)",
             s_num_cached_playlists, s_num_cached_artists);
}

void library_cache_save_playlists(const PlexPlaylist* list, int count) {
    if (!list || count <= 0) return;
    int n = count < CACHE_MAX_ITEMS ? count : CACHE_MAX_ITEMS;
    s_num_cached_playlists = 0;
    for (int i = 0; i < n; i++) {
        if (!list[i].rating_key[0]) continue;
        CachedEntry* e = &s_cached_playlists[s_num_cached_playlists++];
        memset(e, 0, sizeof(*e));
        strncpy(e->rating_key, list[i].rating_key, sizeof(e->rating_key) - 1);
        strncpy(e->title, list[i].title, sizeof(e->title) - 1);
        e->leaf_count = list[i].leaf_count;
    }
    save_cache_file();
}

void library_cache_save_artists(const char* library_key, const PlexArtist* list, int count) {
    if (!list || count <= 0) return;
    strncpy(s_cached_artists_library_key, library_key ? library_key : "", sizeof(s_cached_artists_library_key) - 1);
    int n = count < CACHE_MAX_ITEMS ? count : CACHE_MAX_ITEMS;
    s_num_cached_artists = 0;
    for (int i = 0; i < n; i++) {
        if (!list[i].rating_key[0]) continue;
        CachedEntry* e = &s_cached_artists[s_num_cached_artists++];
        memset(e, 0, sizeof(*e));
        strncpy(e->rating_key, list[i].rating_key, sizeof(e->rating_key) - 1);
        strncpy(e->title, list[i].title, sizeof(e->title) - 1);
    }
    save_cache_file();
}

// A cached entry's `.key` is just its rating_key again, standing in for the
// real online "/children"/"/items" URL - fine, since a cached (fallback)
// list is only ever browsed offline (see ui.c's s_playlists_from_cache/
// s_artists_from_cache), which never issues a live fetch through `.key`
// anyway - only offline_get_*() calls keyed by rating_key.

int library_cache_load_playlists(PlexPlaylist* out, int max) {
    if (!out || max <= 0) return 0;
    int n = s_num_cached_playlists < max ? s_num_cached_playlists : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].rating_key, s_cached_playlists[i].rating_key, sizeof(out[i].rating_key) - 1);
        strncpy(out[i].key, s_cached_playlists[i].rating_key, sizeof(out[i].key) - 1);
        strncpy(out[i].title, s_cached_playlists[i].title, sizeof(out[i].title) - 1);
        out[i].leaf_count = s_cached_playlists[i].leaf_count;
    }
    return n;
}

int library_cache_load_artists(const char* library_key, PlexArtist* out, int max) {
    if (!out || max <= 0 || !library_key) return 0;
    if (strcmp(s_cached_artists_library_key, library_key) != 0) return 0;
    int n = s_num_cached_artists < max ? s_num_cached_artists : max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        strncpy(out[i].rating_key, s_cached_artists[i].rating_key, sizeof(out[i].rating_key) - 1);
        strncpy(out[i].key, s_cached_artists[i].rating_key, sizeof(out[i].key) - 1);
        strncpy(out[i].title, s_cached_artists[i].title, sizeof(out[i].title) - 1);
    }
    return n;
}
