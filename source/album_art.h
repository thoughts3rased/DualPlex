#ifndef ALBUM_ART_H
#define ALBUM_ART_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

void album_art_init(void);
void album_art_cleanup(void);
void album_art_load_async(const char* thumb_path, const char* server_url, const char* auth_token);
// Loads a thumbnail already cached on the SD card - see offline.c's
// per-album thumbnail cache (offline_get_thumb_path()) - for Now Playing art
// while playing back a downloaded track with no server connection to fetch
// from. Synchronous, unlike album_art_load_async().
void album_art_load_local(const char* path);
void album_art_update(void);
void album_art_draw(float x, float y, float w, float h);
bool album_art_has_texture(void);
bool album_art_is_loading(void);

#endif // ALBUM_ART_H
