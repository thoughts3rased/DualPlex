#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "lib/stb_image.h"
#include "album_art.h"
#include "logger.h"

typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} ImageBuf;

static C3D_Tex s_tex;
static Tex3DS_SubTexture s_subtex;
static C2D_Image s_image;
static bool s_has_texture = false;
static bool s_is_loading = false;

static CURLM* s_curl_multi = NULL;
static CURL* s_curl_easy = NULL;
static struct curl_slist* s_headers = NULL;
static ImageBuf s_buf = {0};

static inline u32 get_morton_offset(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static size_t img_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    ImageBuf* mem = (ImageBuf*)userp;
    if (mem->size + realsize + 1 > mem->capacity) {
        size_t new_cap = mem->capacity ? mem->capacity * 2 : 16384;
        while (new_cap < mem->size + realsize + 1) new_cap *= 2;
        char* ptr = (char*)realloc(mem->data, new_cap);
        if (!ptr) return 0;
        mem->data = ptr;
        mem->capacity = new_cap;
    }
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    return realsize;
}

void album_art_init(void) {
    memset(&s_tex, 0, sizeof(s_tex));
    s_has_texture = false;
    s_is_loading = false;
    s_curl_multi = curl_multi_init();
}

void album_art_cleanup(void) {
    if (s_curl_multi && s_curl_easy) {
        curl_multi_remove_handle(s_curl_multi, s_curl_easy);
        curl_easy_cleanup(s_curl_easy);
        s_curl_easy = NULL;
    }
    if (s_headers) {
        curl_slist_free_all(s_headers);
        s_headers = NULL;
    }
    if (s_buf.data) {
        free(s_buf.data);
        s_buf.data = NULL;
    }
    s_buf.size = 0;
    s_buf.capacity = 0;
    
    if (s_has_texture) {
        C3D_TexDelete(&s_tex);
        s_has_texture = false;
    }
    s_is_loading = false;
}

void album_art_load_async(const char* thumb_path, const char* server_url, const char* auth_token) {
    album_art_cleanup();
    if (!thumb_path || !thumb_path[0] || !server_url || !server_url[0]) return;
    
    char url[1024];
    if (thumb_path[0] == '/') {
        snprintf(url, sizeof(url), "%s%s", server_url, thumb_path);
    } else {
        snprintf(url, sizeof(url), "%s/photo/:/transcode?width=128&height=128&minSize=1&url=/library/metadata/%s", server_url, thumb_path);
    }
    
    s_curl_easy = curl_easy_init();
    if (!s_curl_easy) return;
    
    s_buf.capacity = 32768;
    s_buf.data = (char*)malloc(s_buf.capacity);
    s_buf.size = 0;
    if (!s_buf.data) {
        curl_easy_cleanup(s_curl_easy);
        s_curl_easy = NULL;
        return;
    }
    
    char token_header[256];
    snprintf(token_header, sizeof(token_header), "X-Plex-Token: %s", auth_token);
    s_headers = curl_slist_append(NULL, token_header);
    
    curl_easy_setopt(s_curl_easy, CURLOPT_URL, url);
    curl_easy_setopt(s_curl_easy, CURLOPT_HTTPHEADER, s_headers);
    curl_easy_setopt(s_curl_easy, CURLOPT_WRITEFUNCTION, img_write_cb);
    curl_easy_setopt(s_curl_easy, CURLOPT_WRITEDATA, (void*)&s_buf);
    curl_easy_setopt(s_curl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_curl_easy, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(s_curl_easy, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(s_curl_easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s_curl_easy, CURLOPT_SSL_VERIFYHOST, 0L);
    
    if (!s_curl_multi) s_curl_multi = curl_multi_init();
    curl_multi_add_handle(s_curl_multi, s_curl_easy);
    s_is_loading = true;
    LOG_INFO("Started async background download for album art: %s", url);
}

// Decodes raw image bytes (JPEG/PNG - whatever stb_image handles) and, on
// success, uploads them into s_tex/s_image as a 128x128 tiled RGBA8 texture,
// same resampling+rotation as the network path always did. Shared by the
// async network completion handler below and album_art_load_local() (offline
// playback's on-SD-card cached thumbnails) - decoding is identical either
// way, only how the bytes were obtained differs. Does not touch s_is_loading
// (callers manage that themselves, since only the network path has a
// meaningful "loading" state).
static bool decode_and_upload(const u8* data, size_t size) {
    if (!data || size == 0) return false;

    int w = 0, h = 0, channels = 0;
    u8* pixels = stbi_load_from_memory(data, (int)size, &w, &h, &channels, 4);
    if (!pixels) {
        LOG_WARN("stbi_load_from_memory failed on %d bytes", (int)size);
        return false;
    }

    u32 tex_w = 128;
    u32 tex_h = 128;
    if (!C3D_TexInit(&s_tex, tex_w, tex_h, GPU_RGBA8)) {
        stbi_image_free(pixels);
        return false;
    }

    u32* tex_data = (u32*)s_tex.data;
    memset(tex_data, 0, tex_w * tex_h * 4);

    // Resample & rotate 90 deg CCW to counteract 90 deg CW hardware rotation
    for (u32 ty = 0; ty < tex_h; ty++) {
        for (u32 tx = 0; tx < tex_w; tx++) {
            u32 sx = ((tex_h - 1 - ty) * (u32)w) / tex_h;
            u32 sy = (tx * (u32)h) / tex_w;

            u32 src_idx = (sy * (u32)w + sx) * 4;
            u8 r = pixels[src_idx + 0];
            u8 g = pixels[src_idx + 1];
            u8 b = pixels[src_idx + 2];
            u8 a = pixels[src_idx + 3];

            // 3DS PICA200 GPU_RGBA8 format: R, G, B, A
            u32 color = (r << 24) | (g << 16) | (b << 8) | a;

            u32 tile_x = tx / 8;
            u32 tile_y = ty / 8;
            u32 in_tile_x = tx % 8;
            u32 in_tile_y = ty % 8;

            u32 tile_index = (tile_y * (tex_w / 8)) + tile_x;
            u32 pixel_index = (tile_index * 64) + get_morton_offset(in_tile_x, in_tile_y);
            tex_data[pixel_index] = color;
        }
    }
    stbi_image_free(pixels);
    GSPGPU_FlushDataCache(s_tex.data, tex_w * tex_h * 4);

    s_subtex.width = 128;
    s_subtex.height = 128;
    s_subtex.left = 0.0f;
    s_subtex.top = 0.0f;
    s_subtex.right = 1.0f;
    s_subtex.bottom = 1.0f;

    s_image.tex = &s_tex;
    s_image.subtex = &s_subtex;
    s_has_texture = true;
    LOG_INFO("Album art resampled & decoded into 128x128 texture (%dx%d source)", w, h);
    return true;
}

// Loads a thumbnail straight off the SD card (offline.c's per-album cache -
// see offline_get_thumb_path()) instead of over the network - used for the
// Now Playing screen while playing back a downloaded track, where there may
// be no server connection at all to fetch from. Synchronous (the file's
// already local, so there's no "in flight" state to pump), unlike
// album_art_load_async().
void album_art_load_local(const char* path) {
    album_art_cleanup();
    if (!path || !path[0]) return;

    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return;
    }

    u8* buf = (u8*)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (got == (size_t)sz) {
        decode_and_upload(buf, got);
    }
    free(buf);
}

void album_art_update(void) {
    if (!s_is_loading || !s_curl_multi || !s_curl_easy) return;

    int running_handles = 0;
    CURLMcode mres = curl_multi_perform(s_curl_multi, &running_handles);

    if (mres != CURLM_OK || running_handles == 0) {
        s_is_loading = false;
        long http_code = 0;
        curl_easy_getinfo(s_curl_easy, CURLINFO_RESPONSE_CODE, &http_code);

        if (http_code == 200 && s_buf.data && s_buf.size > 0) {
            decode_and_upload((const u8*)s_buf.data, s_buf.size);
        } else {
            LOG_WARN("Async album art HTTP download failed (code=%ld)", http_code);
        }

        curl_multi_remove_handle(s_curl_multi, s_curl_easy);
        curl_easy_cleanup(s_curl_easy);
        s_curl_easy = NULL;
        if (s_headers) {
            curl_slist_free_all(s_headers);
            s_headers = NULL;
        }
    }
}

void album_art_draw(float x, float y, float w, float h) {
    if (!s_has_texture) return;
    float scaleX = w / 128.0f;
    float scaleY = h / 128.0f;
    C2D_DrawImageAt(s_image, x, y, 0.5f, NULL, scaleX, scaleY);
}

bool album_art_has_texture(void) {
    return s_has_texture;
}

bool album_art_is_loading(void) {
    return s_is_loading;
}
