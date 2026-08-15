#ifndef UI_H
#define UI_H

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>
#include "config.h"

typedef enum {
    SCREEN_AUTH_CHOICE,   // Choose authentication method (Link code, Direct login, Manual)
    SCREEN_LINK_PIN,      // Link device via plex.tv/link code
    SCREEN_LOGIN_DIRECT,  // Direct Username + Password + 2FA input
    SCREEN_SERVER_SELECT, // Select server from account resources
    SCREEN_SETUP,         // Manual server URL and token config
    SCREEN_HUB,           // Main Hub (Artists, Playlists, Search, Recently Added)
    SCREEN_LIBRARIES,     // List of music libraries
    SCREEN_ARTISTS,       // List of artists
    SCREEN_ALBUMS,        // List of albums for an artist
    SCREEN_TRACKS,        // List of tracks for an album or playlist
    SCREEN_PLAYLISTS,     // List of playlists
    SCREEN_SEARCH,        // Search tracks screen
    SCREEN_NOW_PLAYING,   // Dedicated Now Playing controls screen
    SCREEN_QUEUE,         // Play Queue / Up Next screen
    SCREEN_LYRICS,        // Time-Synced Lyrics screen
    SCREEN_SETTINGS,      // App-level settings (clock format, etc.)
} UIScreen;

// Initialize UI resources (text buffers, colors, etc.).
void ui_init(void);

// Clean up UI resources.
void ui_cleanup(void);

// Process input and update UI state.
void ui_update(u32 kDown, u32 kHeld, touchPosition touch);

// Render the top screen (now playing info).
void ui_render_top(C3D_RenderTarget* top);

// Render the bottom screen (navigation/controls).
void ui_render_bottom(C3D_RenderTarget* bottom);

// One-off "Connecting..." frame for a blocking startup connection attempt,
// before the main render loop (and its own per-frame ui_render_top()/
// ui_render_bottom() calls) has started. Unlike those two, this clears and
// scene-begins both targets itself - call it directly between
// C3D_FrameBegin()/C3D_FrameEnd(), nothing else needed.
void ui_render_connecting(C3D_RenderTarget* top, C3D_RenderTarget* bottom);

// Get current screen.
UIScreen ui_get_screen(void);

// Set current screen.
void ui_set_screen(UIScreen screen);

// Set AppConfig pointer.
void ui_set_config(AppConfig* config);

#endif // UI_H
