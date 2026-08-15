#include "ui.h"
#include "plex_api.h"
#include "audio_player.h"
#include "album_art.h"
#include "logger.h"
#include "iconfont_bin.h" // bundled Font Awesome Free icon font (SIL OFL 1.1, see licenses/fontawesome-LICENSE.txt), embedded via bin2s from data/iconfont.bin

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Compile-time constant color macro (C2D_Color32 is not constexpr)
#define RGBA8(r, g, b, a) ((u32)((((a)&0xFF)<<24) | (((b)&0xFF)<<16) | (((g)&0xFF)<<8) | ((r)&0xFF)))

#define COL_BG          RGBA8(0x1A, 0x1A, 0x2E, 0xFF)   // dark navy background
#define COL_BG_TOP      RGBA8(0x12, 0x12, 0x24, 0xFF)   // slightly darker for top screen
#define COL_SURFACE     RGBA8(0x25, 0x25, 0x40, 0xFF)   // card/surface color
#define COL_ACCENT      RGBA8(0xE5, 0xA0, 0x0D, 0xFF)   // Plex gold/amber accent
#define COL_ACCENT_DIM  RGBA8(0xB3, 0x7D, 0x0A, 0xFF)   // dimmer accent
#define COL_TEXT        RGBA8(0xEE, 0xEE, 0xF0, 0xFF)   // light text
#define COL_TEXT_DIM    RGBA8(0x88, 0x88, 0xA0, 0xFF)   // dimmed text
#define COL_TEXT_DARK   RGBA8(0x55, 0x55, 0x70, 0xFF)   // very dim text
#define COL_HIGHLIGHT   RGBA8(0x35, 0x35, 0x58, 0xFF)   // selected item highlight
#define COL_PROGRESS_BG RGBA8(0x33, 0x33, 0x50, 0xFF)   // progress bar background
#define COL_PLAYING     RGBA8(0x4C, 0xAF, 0x50, 0xFF)   // green for playing state
#define COL_ERROR       RGBA8(0xE5, 0x39, 0x35, 0xFF)   // red for errors
#define COL_WARN        RGBA8(0xFF, 0x98, 0x00, 0xFF)   // orange for warnings

#define TOP_WIDTH 400
#define TOP_HEIGHT 240
#define BTM_WIDTH 320
#define BTM_HEIGHT 240
#define LIST_ITEM_HEIGHT 32
#define LIST_VISIBLE_ITEMS 6  // items visible on bottom screen
#define LIST_START_Y 44       // below the header

// Static state
static UIScreen s_screen = SCREEN_SETUP;
static C2D_TextBuf s_text_buf;
static C2D_Font s_font;

// List navigation
static int s_list_offset = 0;
static int s_selected_idx = 0;
static int s_list_count = 0;

// Data storage
static PlexLibrary s_libraries[PLEX_MAX_ITEMS];
static PlexArtist s_artists[PLEX_MAX_ITEMS];
static PlexAlbum s_albums[PLEX_MAX_ITEMS];
static PlexTrack s_tracks[PLEX_MAX_ITEMS];
static PlexPlaylist s_playlists[PLEX_MAX_ITEMS];
static int s_num_libraries = 0;
static int s_num_artists = 0;
static int s_num_albums = 0;
static int s_num_tracks = 0;
static int s_num_playlists = 0;
static bool s_need_load_libraries = false;

// Continuous Lazy Loading State
static int s_total_items = 0;
static int s_loaded_items = 0;
static char s_active_key[PLEX_MAX_URL] = "";
static bool s_is_lazy_loading = false;

// Current context
static char s_current_title[PLEX_MAX_STR] = "";
static int s_current_track_idx = -1;
static bool s_auto_advance = true;

// Shuffle/repeat playback state.
static bool s_shuffle_enabled = false;
typedef enum { REPEAT_OFF, REPEAT_ALL, REPEAT_ONE, REPEAT_MODE_COUNT } RepeatMode;
static RepeatMode s_repeat_mode = REPEAT_OFF;
// Shuffle has no well-defined "previous" (it's random), so Prev instead
// steps back through actual play history built up as shuffle advances -
// pushed to in play_next_track(), popped in play_prev_track().
#define SHUFFLE_HISTORY_MAX 32
static int s_shuffle_history[SHUFFLE_HISTORY_MAX];
static int s_shuffle_history_count = 0;
// Index of the track to prefetch once it's safe to do so (-1 = none pending).
// Some Plex servers only permit one concurrent transcode session, so we hold
// off starting the next track's prefetch until the current track's own
// stream has finished downloading - see audio_player_is_download_finished().
static int s_pending_prefetch_idx = -1;
// Track index the currently in-flight/ready prefetch buffer actually holds
// (-1 = none). audio_player_is_prefetch_ready() only reports "is there a
// prefetched buffer", not which track it's for - without checking this too,
// jumping ahead in the queue (e.g. skipping past the natural next track)
// would activate that stale next-track prefetch as if it were the track the
// user actually picked, playing the wrong audio under the right metadata.
static int s_prefetch_track_idx = -1;

// Top-screen view mode, cycled with L/R (see ui_update()).
typedef enum {
    TOPVIEW_NOW_PLAYING,
    TOPVIEW_LYRICS,
    TOPVIEW_VISUALIZER,
    TOPVIEW_COUNT
} TopView;
static TopView s_top_view = TOPVIEW_NOW_PLAYING;

// Visualizer style, cycled with X while TOPVIEW_VISUALIZER is active.
typedef enum {
    VIS_STYLE_BARS,
    VIS_STYLE_OSCILLOSCOPE,
    VIS_STYLE_COUNT
} VisStyle;
static VisStyle s_vis_style = VIS_STYLE_BARS;

// Setup & Auth fields
static AppConfig* s_config = NULL;
static int s_setup_field = 0;

// Auth states
static PlexPin s_pin;
static u32 s_pin_timer = 0;
static char s_login_user[128] = "";
static char s_login_pass[128] = "";
static char s_login_2fa[16] = "";
static PlexServerResource s_servers[PLEX_MAX_ITEMS];
static int s_num_servers = 0;

// Status message
static char s_status_msg[256] = "";
static u32 s_status_color = COL_TEXT_DIM;

#define PLEX_MAX_LYRICS 64
static PlexLyricLine s_lyrics[PLEX_MAX_LYRICS];
static int s_num_lyrics = 0;
// Smoothly-animated scroll position for the lyrics views, shared by both
// (top-screen and bottom-screen) since they represent the same underlying
// state. Eases toward find_scroll_anchor_line()'s target each frame instead
// of snapping straight to it. s_lyrics_scroll_gen tracks which "set" of
// lyrics (i.e. which track) the animated position belongs to, so loading a
// new track's lyrics snaps instantly instead of visibly sliding from the
// old song's line index to the new one's.
static float s_lyrics_scroll_pos = 0.0f;
static int s_lyrics_scroll_gen = -1;
static int s_lyrics_load_id = 0; // bumped each time a new track's lyrics are loaded

// Live Log Viewer Overlay
static bool s_show_logs = false;
static int s_log_scroll_offset = 0;

void ui_init(void) {
    s_text_buf = C2D_TextBufNew(8192);
    s_font = C2D_FontLoadFromMem(iconfont_bin, iconfont_bin_size);
    if (!s_font) {
        LOG_ERROR("Failed to load bundled icon font - playback icons will not render");
    }
    album_art_init();
    LOG_INFO("UI system initialized");
}

void ui_cleanup(void) {
    if (s_font) {
        C2D_FontFree(s_font);
        s_font = NULL;
    }
    album_art_cleanup();
    C2D_TextBufDelete(s_text_buf);
    LOG_INFO("UI system cleaned up");
}

void ui_set_config(AppConfig* config) {
    s_config = config;
}

UIScreen ui_get_screen(void) {
    return s_screen;
}

void ui_set_screen(UIScreen screen) {
    s_screen = screen;
    s_list_offset = 0;
    s_selected_idx = 0;
    
    if (screen == SCREEN_AUTH_CHOICE) {
        s_list_count = 3;
        s_setup_field = 0;
    } else if (screen == SCREEN_LINK_PIN) {
        snprintf(s_status_msg, sizeof(s_status_msg), "Creating PIN...");
        s_status_color = COL_TEXT;
        if (!plex_api_create_pin(&s_pin)) {
            snprintf(s_status_msg, sizeof(s_status_msg), "Failed to generate PIN. Check connection.");
            s_status_color = COL_ERROR;
        } else {
            snprintf(s_status_msg, sizeof(s_status_msg), "Waiting for authorization...");
            s_status_color = COL_ACCENT;
        }
        s_pin_timer = 0;
        s_list_count = 0;
    } else if (screen == SCREEN_LOGIN_DIRECT) {
        s_setup_field = 0;
        s_list_count = 4;
        snprintf(s_status_msg, sizeof(s_status_msg), "Enter your Plex account details.");
        s_status_color = COL_TEXT_DIM;
    } else if (screen == SCREEN_SERVER_SELECT) {
        if (s_num_servers == 0 && s_config->auth_token[0]) {
            s_num_servers = plex_api_get_servers(s_config->auth_token, s_servers, PLEX_MAX_ITEMS);
        }
        s_list_count = s_num_servers;
    } else if (screen == SCREEN_SETUP) {
        s_setup_field = 0;
        s_list_count = 3;
    } else if (screen == SCREEN_HUB) {
        s_list_count = 5;
    } else if (screen == SCREEN_PLAYLISTS) {
        s_num_playlists = plex_api_get_playlists(s_playlists, PLEX_MAX_ITEMS);
        s_list_count = s_num_playlists;
    } else if (screen == SCREEN_LIBRARIES) {
        s_list_count = s_num_libraries;
        if (s_num_libraries == 0) {
            s_need_load_libraries = true;
        }
    } else if (screen == SCREEN_ARTISTS) {
        s_list_count = (s_total_items > 0) ? s_total_items : s_num_artists;
    } else if (screen == SCREEN_ALBUMS) {
        s_list_count = (s_total_items > 0) ? s_total_items : s_num_albums;
    } else if (screen == SCREEN_TRACKS) {
        s_list_count = (s_total_items > 0) ? s_total_items : s_num_tracks;
    } else {
        s_list_count = 0;
    }
}

static void draw_text(const char* str, float x, float y, float scaleX, float scaleY, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, s_text_buf, str);
    C2D_TextOptimize(&text);
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scaleX, scaleY, color);
}

static void draw_text_centered(const char* str, float y, float width, float scaleX, float scaleY, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, s_text_buf, str);
    C2D_TextOptimize(&text);
    float tw, th;
    C2D_TextGetDimensions(&text, scaleX, scaleY, &tw, &th);
    float x = (width - tw) / 2.0f;
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scaleX, scaleY, color);
}

// Like draw_text_centered(), but centers within [region_x, region_x+width]
// instead of always starting at 0 - for centering within a column rather
// than the full screen.
static void draw_text_centered_at(const char* str, float region_x, float y, float width, float scaleX, float scaleY, u32 color) {
    C2D_Text text;
    C2D_TextParse(&text, s_text_buf, str);
    C2D_TextOptimize(&text);
    float tw, th;
    C2D_TextGetDimensions(&text, scaleX, scaleY, &tw, &th);
    float x = region_x + (width - tw) / 2.0f;
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scaleX, scaleY, color);
}

static void draw_scrolling_text(const char* str, float x, float y, float max_w, float scaleX, float scaleY, u32 color) {
    if (!str || !str[0]) return;
    
    C2D_Text text;
    C2D_TextParse(&text, s_text_buf, str);
    C2D_TextOptimize(&text);
    
    float tw = 0, th = 0;
    C2D_TextGetDimensions(&text, scaleX, scaleY, &tw, &th);
    
    if (tw <= max_w) {
        C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scaleX, scaleY, color);
        return;
    }
    
    static float s_scroll_timer = 0.0f;
    s_scroll_timer += 0.016f;
    float max_scroll = tw + 50.0f;
    float scroll_offset = fmodf(s_scroll_timer * 25.0f, max_scroll);
    
    float draw_x = x - scroll_offset;
    if (draw_x >= x) {
        C2D_DrawText(&text, C2D_WithColor, draw_x, y, 0.5f, scaleX, scaleY, color);
    } else {
        int str_len = (int)strlen(str);
        int offset = (int)((scroll_offset / tw) * str_len);
        if (offset >= str_len) offset = 0;
        
        C2D_Text subtext;
        C2D_TextParse(&subtext, s_text_buf, str + offset);
        C2D_TextOptimize(&subtext);
        C2D_DrawText(&subtext, C2D_WithColor, x, y, 0.5f, scaleX, scaleY, color);
    }
}

// Word-wraps str to fit within max_width, drawing each resulting row centered
// within [region_x, region_x+max_width] and stopping once max_height is
// used up (rows that would overflow it are simply not drawn, rather than
// spilling past the caller's box). Returns how many rows were actually
// drawn (0 if str is empty or nothing fit) so callers stacking multiple
// wrapped blocks - e.g. lyric lines - can advance by the real height instead
// of assuming a fixed one-line height and overlapping the next block.
static int draw_text_wrapped_at(const char* str, float region_x, float start_y, float max_width, float max_height, float scaleX, float scaleY, u32 color) {
    if (!str || !str[0]) return 0;

    char clean[512];
    strncpy(clean, str, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    for (int i = 0; clean[i]; i++) {
        if (clean[i] == '\n' || clean[i] == '\r' || clean[i] == '\t') clean[i] = ' ';
    }

    char line[128] = "";
    char word[64] = "";
    float cur_y = start_y;
    float line_h = 20.0f * scaleY;
    int rows = 0;

    const char* p = clean;
    while (*p && (cur_y + line_h <= start_y + max_height)) {
        while (*p == ' ') p++;
        if (!*p) break;

        int wlen = 0;
        while (*p && *p != ' ' && wlen < (int)sizeof(word) - 1) {
            word[wlen++] = *p++;
        }
        word[wlen] = '\0';

        char test[256];
        if (line[0] == '\0') {
            snprintf(test, sizeof(test), "%s", word);
        } else {
            snprintf(test, sizeof(test), "%s %s", line, word);
        }

        C2D_Text txt;
        C2D_TextParse(&txt, s_text_buf, test);
        C2D_TextOptimize(&txt);
        float tw, th;
        C2D_TextGetDimensions(&txt, scaleX, scaleY, &tw, &th);

        if (tw > max_width && line[0] != '\0') {
            draw_text_centered_at(line, region_x, cur_y, max_width, scaleX, scaleY, color);
            rows++;
            cur_y += line_h;
            strncpy(line, word, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
        } else {
            strncpy(line, test, sizeof(line) - 1);
            line[sizeof(line) - 1] = '\0';
        }
    }
    if (line[0] != '\0' && (cur_y + line_h <= start_y + max_height)) {
        draw_text_centered_at(line, region_x, cur_y, max_width, scaleX, scaleY, color);
        rows++;
    }
    return rows;
}

static float s_spinner_angle = 0.0f;

static void draw_loading_spinner(float cx, float cy, float radius, const char* label) {
    s_spinner_angle += 0.12f;
    if (s_spinner_angle > 6.28318f) s_spinner_angle -= 6.28318f;
    
    C2D_DrawCircleSolid(cx, cy, 0.5f, radius + 2, COL_SURFACE);
    C2D_DrawCircleSolid(cx, cy, 0.5f, radius - 4, COL_BG);
    
    for (int i = 0; i < 8; i++) {
        float a = s_spinner_angle + i * (6.28318f / 8.0f);
        float dot_x = cx + cosf(a) * (radius - 1);
        float dot_y = cy + sinf(a) * (radius - 1);
        float dot_size = 2.0f + (i * 0.4f);
        u32 dot_col = C2D_Color32(0xE5, 0xA0, 0x0D, 30 + i * 28);
        C2D_DrawCircleSolid(dot_x, dot_y, 0.5f, dot_size, dot_col);
    }
    
    if (label && label[0]) {
        draw_text_centered(label, cy + radius + 15, BTM_WIDTH, 0.5f, 0.5f, COL_TEXT_DIM);
    }
}

// --- Playback control icons -------------------------------------------
// Rendered from a bundled icon font (Font Awesome Free 6.5.1, solid style;
// SIL OFL 1.1, see licenses/fontawesome-LICENSE.txt) rather than plain
// Unicode symbols (▶ ⏸ ⏭ etc.) or emoji, since the console's system font
// has no glyphs for those and falls back to "?" - same issue as the
// pre-existing "⚡" charging indicator and "◀" back arrow elsewhere in this
// file (left as hand-drawn shapes; not part of this icon set).
// Only the specific glyphs actually used are baked into the .bcfnt
// (data/iconfont.bin, embedded directly into the binary - see the Makefile's
// bin2o rule - so no RomFS is needed to ship it).
#define ICON_CP_PLAY           0xF04B
#define ICON_CP_PAUSE          0xF04C
#define ICON_CP_BACKWARD_STEP  0xF048
#define ICON_CP_FORWARD_STEP   0xF051
#define ICON_CP_SHUFFLE        0xF074
#define ICON_CP_REPEAT         0xF363

// Every codepoint above sits in Font Awesome's Private Use Area, always a
// 3-byte UTF-8 sequence (U+0800-U+FFFF). Encoded manually rather than
// embedding raw UTF-8 bytes as source literals, so the codepoint value stays
// self-evident and nothing depends on this file's saved encoding.
static void utf8_encode_icon(u32 cp, char* out) {
    out[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
}

// Shared renderer for the icon-font glyphs above, centered on (cx, cy) and
// scaled so the glyph is roughly `size` pixels tall (the font was baked at
// 24px). Falls back to nothing drawn if the bundled font failed to load.
static void draw_icon_glyph(u32 codepoint, float cx, float cy, float size, u32 color) {
    if (!s_font) return;
    char utf8[4];
    utf8_encode_icon(codepoint, utf8);
    C2D_Text text;
    C2D_TextFontParse(&text, s_font, s_text_buf, utf8);
    C2D_TextOptimize(&text);
    float scale = size / 24.0f;
    float w, h;
    C2D_TextGetDimensions(&text, scale, scale, &w, &h);
    C2D_DrawText(&text, C2D_WithColor, cx - w / 2.0f, cy - h / 2.0f, 0.5f, scale, scale, color);
}

static void draw_icon_play(float cx, float cy, float size, u32 color) {
    draw_icon_glyph(ICON_CP_PLAY, cx, cy, size, color);
}

static void draw_icon_pause(float cx, float cy, float size, u32 color) {
    draw_icon_glyph(ICON_CP_PAUSE, cx, cy, size, color);
}

static void draw_icon_skip_next(float cx, float cy, float size, u32 color) {
    draw_icon_glyph(ICON_CP_FORWARD_STEP, cx, cy, size, color);
}

static void draw_icon_skip_prev(float cx, float cy, float size, u32 color) {
    draw_icon_glyph(ICON_CP_BACKWARD_STEP, cx, cy, size, color);
}

static void draw_icon_shuffle(float cx, float cy, float size, u32 color) {
    draw_icon_glyph(ICON_CP_SHUFFLE, cx, cy, size, color);
}

// Font Awesome's free set has no distinct "repeat-one" glyph, so repeat-one
// draws the regular repeat glyph with a small "1" (baked into the same font)
// beside it, same idea as the previous hand-drawn version.
static void draw_icon_repeat(float cx, float cy, float size, u32 color, bool repeat_one) {
    if (repeat_one) {
        draw_icon_glyph(ICON_CP_REPEAT, cx - size * 0.22f, cy, size * 0.9f, color);
        if (s_font) {
            char utf8[4];
            utf8_encode_icon('1', utf8);
            C2D_Text text;
            C2D_TextFontParse(&text, s_font, s_text_buf, utf8);
            C2D_TextOptimize(&text);
            float scale = (size * 0.5f) / 24.0f;
            float w, h;
            C2D_TextGetDimensions(&text, scale, scale, &w, &h);
            float gx = cx + size * 0.42f, gy = cy + size * 0.12f;
            C2D_DrawText(&text, C2D_WithColor, gx - w / 2.0f, gy - h / 2.0f, 0.5f, scale, scale, color);
        }
    } else {
        draw_icon_glyph(ICON_CP_REPEAT, cx, cy, size, color);
    }
}

static void draw_icon_back_arrow(float cx, float cy, float size, u32 color) {
    float h = size, w = size * 0.85f;
    C2D_DrawTriangle(cx + w * 0.4f, cy - h * 0.5f, color,
                      cx + w * 0.4f, cy + h * 0.5f, color,
                      cx - w * 0.6f, cy, color, 0.5f);
}

static void draw_header(const char* title) {
    C2D_DrawRectSolid(0, 0, 0.5f, BTM_WIDTH, 40, COL_SURFACE);
    C2D_DrawRectSolid(0, 40, 0.5f, BTM_WIDTH, 2, COL_ACCENT);
    draw_text(title, 8, 10, 0.6f, 0.6f, COL_TEXT);
    if (s_screen != SCREEN_SETUP && s_screen != SCREEN_LIBRARIES) {
        draw_icon_back_arrow(BTM_WIDTH - 78, 18, 10, COL_TEXT_DIM);
        draw_text("B: Back", BTM_WIDTH - 68, 12, 0.45f, 0.45f, COL_TEXT_DIM);
    }
}

static void draw_list_item(int visual_idx, const char* title, const char* subtitle, bool selected, bool is_playing) {
    float y = LIST_START_Y + visual_idx * LIST_ITEM_HEIGHT;
    u32 bg_col = selected ? COL_HIGHLIGHT : ((visual_idx % 2 == 0) ? COL_BG : COL_SURFACE);
    C2D_DrawRectSolid(0, y, 0.5f, BTM_WIDTH, LIST_ITEM_HEIGHT - 1, bg_col);
    
    if (is_playing) {
        C2D_DrawRectSolid(0, y, 0.5f, 3, LIST_ITEM_HEIGHT - 1, COL_ACCENT);
    }
    
    draw_text(title, 10, y + 2, 0.5f, 0.5f, selected ? COL_ACCENT : COL_TEXT);
    if (subtitle && subtitle[0]) {
        draw_text(subtitle, 10, y + 16, 0.4f, 0.4f, COL_TEXT_DIM);
    }
}

// Reads the physical volume slider's position (0-63 on real hardware),
// not the app's own playback volume - shown in the HUD so it reflects what
// the slider is actually set to, the way the system UI does.
static int get_hw_volume_percent(void) {
    u8 slider = 0;
    if (R_SUCCEEDED(HIDUSER_GetSoundVolume(&slider))) {
        return (int)((slider * 100) / 63);
    }
    return 0;
}

// Draws a Home-Menu-style WiFi signal indicator: 3 bars of increasing height,
// filled left-to-right up to the current signal strength (0-3, straight from
// the OS - no service init needed, same value the system's own WiFi icon
// uses). 0 bars filled means no/negligible signal.
static void draw_wifi_indicator(float x, float baseline_y) {
    u8 strength = osGetWifiStrength();
    static const float bar_heights[3] = { 5.0f, 9.0f, 13.0f };
    static const float bar_w = 3.0f;
    static const float bar_gap = 2.0f;

    float bx = x;
    for (int i = 0; i < 3; i++) {
        float h = bar_heights[i];
        u32 col = (i < strength) ? COL_ACCENT : COL_PROGRESS_BG;
        C2D_DrawRectSolid(bx, baseline_y - h, 0.5f, bar_w, h, col);
        bx += bar_w + bar_gap;
    }
}

static void format_time(int ms, char* buf, size_t buf_size) {
    int total_sec = ms / 1000;
    int min = total_sec / 60;
    int sec = total_sec % 60;
    snprintf(buf, buf_size, "%d:%02d", min, sec);
}

static void format_quality_tag(const PlexTrack* track, char* buf, size_t max) {
    if (!track || track->audio_codec[0] == '\0') {
        buf[0] = '\0';
        return;
    }
    
    char codec_str[24] = "";
    if (strcasecmp(track->audio_codec, "flac") == 0) {
        if (track->bit_depth == 24 && track->sampling_rate >= 88200) {
            snprintf(codec_str, sizeof(codec_str), "FLAC 24/%dk", track->sampling_rate / 1000);
        } else if (track->bit_depth > 0) {
            snprintf(codec_str, sizeof(codec_str), "FLAC %dbit", track->bit_depth);
        } else {
            snprintf(codec_str, sizeof(codec_str), "FLAC");
        }
    } else if (strcasecmp(track->audio_codec, "mp3") == 0) {
        if (track->bitrate > 0) snprintf(codec_str, sizeof(codec_str), "MP3 %dk", track->bitrate);
        else snprintf(codec_str, sizeof(codec_str), "MP3");
    } else if (strcasecmp(track->audio_codec, "aac") == 0) {
        if (track->bitrate > 0) snprintf(codec_str, sizeof(codec_str), "AAC %dk", track->bitrate);
        else snprintf(codec_str, sizeof(codec_str), "AAC");
    } else if (strcasecmp(track->audio_codec, "alac") == 0) {
        snprintf(codec_str, sizeof(codec_str), "ALAC %dbit", track->bit_depth > 0 ? track->bit_depth : 16);
    } else {
        snprintf(codec_str, sizeof(codec_str), "%s", track->audio_codec);
    }
    
    snprintf(buf, max, "[%s]", codec_str);
}

// Index of the lyric line whose timestamp has most recently passed - used to
// anchor the scroll position, so the view stays parked near the last-sung
// line through gaps rather than jumping around. Always returns a valid
// index once there's any lyric data (clamped to line 0 before the first
// line's timestamp), including during instrumental breaks - see
// find_active_lyric_line() below for whether anything should actually be
// *highlighted* right now, which is a different question.
static int find_scroll_anchor_line(int pos_ms) {
    int line = 0;
    for (int i = 0; i < s_num_lyrics; i++) {
        if (pos_ms >= s_lyrics[i].time_ms) {
            line = i;
        }
    }
    return line;
}

// Longest an instrumental gap is allowed to hold a line's highlight before
// it's treated as "nothing is actually being sung right now". LRC only
// gives each line a start time, not a duration, so there's no exact way to
// know when a line's vocal actually ends - this is a reasonable upper bound
// on how long one line takes to sing. Covers both a long intro before the
// first line, and mid-song instrumental breaks.
#define LYRIC_MAX_HOLD_MS 6000

// Index of the lyric line that should be shown as "currently playing" right
// now, or -1 if none should be (before the first line starts, or deep into
// an instrumental break between two lines).
static int find_active_lyric_line(int pos_ms) {
    if (s_num_lyrics == 0 || pos_ms < s_lyrics[0].time_ms) return -1;

    int line = find_scroll_anchor_line(pos_ms);

    // Plex marks known instrumental breaks explicitly: a timed entry with no
    // text (see parse_lyrics_stream_response()'s comment on Line/Span). When
    // one's present, trust it outright instead of guessing from timing - it's
    // exact, where the fallback below is not.
    if (s_lyrics[line].text[0] == '\0') return -1;

    // Fallback for lines/providers with no explicit break marker: LRC only
    // gives each line a start time, not a duration, so infer a break from an
    // unusually long gap to the next real line instead.
    int line_start = s_lyrics[line].time_ms;
    int next_start = (line + 1 < s_num_lyrics) ? s_lyrics[line + 1].time_ms : -1;
    int gap = (next_start >= 0) ? (next_start - line_start) : -1;

    // gap < 0 means this is the last line (always held till the song ends);
    // gap <= the grace window means a normal line-to-line handoff, held
    // exactly until the next line as before. Only a long gap that's been
    // running longer than the grace window clears the highlight.
    if (gap >= 0 && gap > LYRIC_MAX_HOLD_MS && (pos_ms - line_start) > LYRIC_MAX_HOLD_MS) {
        return -1;
    }
    return line;
}

static u32 lerp_color(u32 c1, u32 c2, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    u8 r1 = c1 & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = (c1 >> 16) & 0xFF, a1 = (c1 >> 24) & 0xFF;
    u8 r2 = c2 & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = (c2 >> 16) & 0xFF, a2 = (c2 >> 24) & 0xFF;
    u8 r = (u8)(r1 + (r2 - r1) * t);
    u8 g = (u8)(g1 + (g2 - g1) * t);
    u8 b = (u8)(b1 + (b2 - b1) * t);
    u8 a = (u8)(a1 + (a2 - a1) * t);
    return RGBA8(r, g, b, a);
}

// Advances the shared smooth-scroll position toward target_line, snapping
// instantly instead if the lyrics were (re)loaded since the last call - a
// newly-loaded track's lyrics shouldn't visibly slide in from the old
// song's line index.
static float update_lyrics_scroll(int target_line) {
    if (s_lyrics_scroll_gen != s_lyrics_load_id) {
        s_lyrics_scroll_pos = (float)target_line;
        s_lyrics_scroll_gen = s_lyrics_load_id;
    } else {
        s_lyrics_scroll_pos += ((float)target_line - s_lyrics_scroll_pos) * 0.2f;
    }
    return s_lyrics_scroll_pos;
}

// Draws a scrolling, smoothly-sliding window of lyric lines around the
// current position, instead of the line-to-line hard snap this used to do.
// half_window lines are shown above and below center. Each line's font size
// and color blend continuously between dim_scale/dim_col (at rest) and
// active_scale/active_col (exactly at the highlighted line) based on how
// close it is to center - so a line grows/brightens in as it becomes
// current and shrinks/dims back out as it's passed, rather than the size
// and color also hard-snapping alongside the position.
static void draw_lyrics_block(int pos_ms, float region_x, float base_y, float width, float row_h, int half_window,
                               float active_scale, float dim_scale, u32 active_col, u32 dim_col) {
    if (s_num_lyrics == 0) return;

    int anchor = find_scroll_anchor_line(pos_ms);
    int active = find_active_lyric_line(pos_ms);
    float center = update_lyrics_scroll(anchor);

    int lo = (int)floorf(center) - half_window - 1;
    int hi = (int)ceilf(center) + half_window + 1;
    for (int i = lo; i <= hi; i++) {
        if (i < 0 || i >= s_num_lyrics) continue;

        float rel = (float)i - center;
        if (rel < -half_window || rel > half_window) continue;
        float ly = base_y + (rel + half_window) * row_h;

        // Only the line that's genuinely active right now (not just nearest
        // to the scroll center - see find_active_lyric_line()) blends toward
        // the highlighted style. During an instrumental break, active is -1
        // and every line renders at rest, even the one parked at center.
        float t = 1.0f - fabsf(rel);
        if (t < 0.0f) t = 0.0f;
        float active_t = (i == active) ? t : 0.0f;

        float scale = dim_scale + (active_scale - dim_scale) * active_t;
        u32 col = lerp_color(dim_col, active_col, active_t);

        draw_text_wrapped_at(s_lyrics[i].text, region_x, ly, width, row_h * 1.4f, scale, scale, col);
    }
}

static bool show_keyboard(const char* hint, char* buf, size_t buf_size) {
    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, buf_size - 1);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetInitialText(&swkbd, buf);
    SwkbdButton button = swkbdInputText(&swkbd, buf, buf_size);
    return (button != SWKBD_BUTTON_NONE && button != SWKBD_BUTTON_LEFT);
}

static void shuffle_history_push(int idx) {
    if (s_shuffle_history_count >= SHUFFLE_HISTORY_MAX) {
        memmove(s_shuffle_history, s_shuffle_history + 1, (SHUFFLE_HISTORY_MAX - 1) * sizeof(int));
        s_shuffle_history_count = SHUFFLE_HISTORY_MAX - 1;
    }
    s_shuffle_history[s_shuffle_history_count++] = idx;
}

// Track to advance to going forward from s_current_track_idx, respecting
// shuffle and repeat. -1 means stop (end of queue, repeat off).
static int compute_next_track_idx(void) {
    if (s_num_tracks == 0) return -1;
    if (s_repeat_mode == REPEAT_ONE) return s_current_track_idx;

    if (s_shuffle_enabled) {
        if (s_num_tracks == 1) return s_current_track_idx;
        int idx, tries = 0;
        do {
            idx = rand() % s_num_tracks;
        } while (idx == s_current_track_idx && ++tries < 20);
        return idx;
    }

    if (s_current_track_idx + 1 < s_num_tracks) return s_current_track_idx + 1;
    if (s_repeat_mode == REPEAT_ALL) return 0;
    return -1;
}

// Track to go back to (linear order / repeat-all wrap only - shuffle mode
// instead steps back through s_shuffle_history, handled in play_prev_track()
// since that needs to pop the stack, not just read it).
static int compute_prev_track_idx(void) {
    if (s_current_track_idx > 0) return s_current_track_idx - 1;
    if (s_repeat_mode == REPEAT_ALL && s_num_tracks > 0) return s_num_tracks - 1;
    return -1;
}

static void play_track(int idx) {
    if (idx < 0 || idx >= s_num_tracks) return;
    char url[PLEX_MAX_URL];
    
    // Adaptive quality: suggest tier based on measured download speed
    int speed = audio_player_get_download_speed();
    if (speed > 0) {
        bool is_n3ds = false;
        APT_CheckNew3DS(&is_n3ds);
        PlexQualityTier suggested = plex_api_suggest_quality(speed, is_n3ds);
        PlexQualityTier current = plex_api_get_quality_tier();
        // Only auto-upgrade if current tier was set by auto-negotiation (don't override manual)
        // Allow upgrading by at most one tier at a time to be conservative
        if (suggested < current && current > 0) {
            plex_api_set_quality_tier(current - 1);
            LOG_INFO("Adaptive quality: upgrading to %s (speed=%d KB/s)",
                     plex_api_get_quality_label(current - 1), speed / 1024);
        }
    }
    
    // Try seamless transition via prefetch - only if the buffered prefetch is
    // actually for the track being requested (it's always started for
    // whatever was the natural next track at the time; jumping ahead in the
    // queue past that must fall through to a fresh load below instead of
    // playing the wrong, already-buffered track under idx's metadata).
    if (idx == s_prefetch_track_idx && audio_player_is_prefetch_ready()) {
        // Build the URL we'd use for this track to compare with prefetch
        if (plex_api_get_stream_url(&s_tracks[idx], url, sizeof(url))) {
            // Activate the prefetch (it becomes the main stream)
            if (audio_player_activate_prefetch()) {
                audio_player_set_duration(s_tracks[idx].duration);
                s_current_track_idx = idx;
                s_auto_advance = true;
                
                LOG_INFO("⚡ Seamless transition to track %d: %s", idx, s_tracks[idx].title);
                plex_api_report_timeline_async(s_tracks[idx].rating_key, "playing", 0, s_tracks[idx].duration);
                
                if (s_tracks[idx].thumb[0] != '\0' && s_config) {
                    album_art_load_async(s_tracks[idx].thumb, s_config->server_url, s_config->auth_token);
                } else {
                    album_art_cleanup();
                }
                
                s_num_lyrics = 0;
                plex_api_lyrics_async_start(s_tracks[idx].rating_key);

                // Defer prefetching the NEXT track until this one's stream has
                // finished downloading (see s_pending_prefetch_idx above).
                // In shuffle mode this is only a *prediction* (the real pick
                // happens again, independently, when we actually advance) -
                // if it doesn't match, the prefetch just goes unused rather
                // than being played (see play_track()'s prefetch-index check
                // above), so a mismatch costs bandwidth, not correctness.
                s_pending_prefetch_idx = compute_next_track_idx();
                return;  // Done - seamless transition complete
            }
        }
    }
    
    // Normal path: no prefetch available (or it was for a different track),
    // load from scratch. audio_player_load_url() cancels any prefetch in
    // flight, so that buffer - if any - is no longer valid for anything.
    if (plex_api_get_stream_url(&s_tracks[idx], url, sizeof(url))) {
        audio_player_set_duration(s_tracks[idx].duration);
        audio_player_load_url(url);
        s_prefetch_track_idx = -1;
        s_current_track_idx = idx;
        s_auto_advance = true;
        
        LOG_INFO("Playing track %d: %s", idx, s_tracks[idx].title);
        plex_api_report_timeline_async(s_tracks[idx].rating_key, "playing", 0, s_tracks[idx].duration);
        
        // Start non-blocking async background download of album cover art
        if (s_tracks[idx].thumb[0] != '\0' && s_config) {
            album_art_load_async(s_tracks[idx].thumb, s_config->server_url, s_config->auth_token);
        } else {
            album_art_cleanup();
        }
        
        s_num_lyrics = 0;
        plex_api_lyrics_async_start(s_tracks[idx].rating_key);

        // Defer prefetching the NEXT track until this one's stream has
        // finished downloading (see s_pending_prefetch_idx above). In shuffle
        // mode this is only a prediction - see the comment on the identical
        // line above in the seamless-transition branch.
        s_pending_prefetch_idx = compute_next_track_idx();
    }
}

// Advances to the next track per compute_next_track_idx() (shuffle/repeat
// aware). Returns false if there's nowhere to go (end of queue, repeat off) -
// caller decides what that means (stop vs. leave as-is).
static bool play_next_track(void) {
    if (s_current_track_idx < 0) return false;
    int next = compute_next_track_idx();
    if (next < 0) return false;
    if (s_shuffle_enabled && next != s_current_track_idx) {
        shuffle_history_push(s_current_track_idx);
    }
    play_track(next);
    return true;
}

// Goes back a track: in shuffle mode, pops actual play history (shuffle has
// no well-defined "previous" otherwise); linear mode just steps back by one
// (or wraps to the last track under repeat-all). Returns false if there's
// nowhere to go.
static bool play_prev_track(void) {
    if (s_current_track_idx < 0) return false;
    if (s_shuffle_enabled && s_shuffle_history_count > 0) {
        play_track(s_shuffle_history[--s_shuffle_history_count]);
        return true;
    }
    int prev = compute_prev_track_idx();
    if (prev < 0) return false;
    play_track(prev);
    return true;
}

// Seeks the currently-playing track to target_ms by reloading its stream from
// that offset (Plex handles the actual seeking server-side - see
// plex_api_get_seek_url()). Any in-flight prefetch for the next track is
// unaffected since it uses a different session id.
static void seek_to(int target_ms) {
    if (s_current_track_idx < 0 || s_current_track_idx >= s_num_tracks) return;
    PlexTrack* track = &s_tracks[s_current_track_idx];

    if (target_ms < 0) target_ms = 0;
    if (track->duration > 0 && target_ms > track->duration) target_ms = track->duration;

    char url[PLEX_MAX_URL];
    if (!plex_api_get_seek_url(track, target_ms, url, sizeof(url))) return;

    audio_player_set_duration(track->duration);
    audio_player_load_url(url); // cancels any in-flight prefetch
    s_prefetch_track_idx = -1;
    audio_player_set_position_offset_ms(target_ms);
    s_auto_advance = true;

    LOG_INFO("Seeked track %d to %dms", s_current_track_idx, target_ms);
    plex_api_report_timeline_async(track->rating_key, "playing", target_ms, track->duration);
}

static UIScreen s_prev_screen = SCREEN_HUB;

void ui_update(u32 kDown, u32 kHeld, touchPosition touch) {
    album_art_update();
    plex_api_timeline_async_update();
    plex_api_lyrics_async_update();
    if (plex_api_lyrics_async_is_done()) {
        s_num_lyrics = plex_api_lyrics_async_take_result(s_lyrics, PLEX_MAX_LYRICS);
        s_lyrics_load_id++;
    }

    // Toggle live log viewer overlay with L + R combo
    if (((kHeld & KEY_L) && (kDown & KEY_R)) || ((kHeld & KEY_R) && (kDown & KEY_L))) {
        s_show_logs = !s_show_logs;
        s_log_scroll_offset = 0;
    }

    if (s_show_logs) {
        if ((kDown & KEY_B) || (kDown & KEY_TOUCH && touch.px > 0 && touch.py < 40)) {
            s_show_logs = false;
            return;
        }
        int total_logs = logger_get_log_count();
        static int s_scroll_repeat = 0;
        
        bool press_up = (kDown & KEY_UP) || (kDown & KEY_CSTICK_UP);
        bool press_dn = (kDown & KEY_DOWN) || (kDown & KEY_CSTICK_DOWN);
        bool hold_up = (kHeld & KEY_UP) || (kHeld & KEY_CSTICK_UP);
        bool hold_dn = (kHeld & KEY_DOWN) || (kHeld & KEY_CSTICK_DOWN);
        
        if (press_up) {
            if (s_log_scroll_offset > 0) s_log_scroll_offset--;
        } else if (hold_up) {
            s_scroll_repeat++;
            if (s_scroll_repeat > 8) {
                if (s_log_scroll_offset > 0) s_log_scroll_offset--;
                s_scroll_repeat = 6;
            }
        }
        
        if (press_dn) {
            if (s_log_scroll_offset + 1 < total_logs) s_log_scroll_offset++;
        } else if (hold_dn) {
            s_scroll_repeat++;
            if (s_scroll_repeat > 8) {
                if (s_log_scroll_offset + 1 < total_logs) s_log_scroll_offset++;
                s_scroll_repeat = 6;
            }
        }
        
        if (!hold_up && !hold_dn) {
            s_scroll_repeat = 0;
        }
        return;
    }

    PlayerState pstate = audio_player_get_state();
    
    // Periodic timeline reporting to Plex Media Server (every 5 seconds while playing)
    if (pstate == PLAYER_PLAYING && s_current_track_idx >= 0 && s_current_track_idx < s_num_tracks) {
        static u64 s_last_timeline_tick = 0;
        u64 now_tick = svcGetSystemTick();
        if (now_tick - s_last_timeline_tick >= 1340617400ULL) { // 5 sec @ 268MHz
            s_last_timeline_tick = now_tick;
            plex_api_report_timeline_async(
                s_tracks[s_current_track_idx].rating_key,
                "playing",
                audio_player_get_position_ms(),
                s_tracks[s_current_track_idx].duration
            );
        }
    }
    
    // Fire the deferred next-track prefetch once the current track's own
    // stream has finished downloading (see s_pending_prefetch_idx above) -
    // starting it any earlier would race the current track for the server's
    // single transcode slot and can 400 either request.
    if (s_pending_prefetch_idx >= 0 && audio_player_is_download_finished()) {
        int next_idx = s_pending_prefetch_idx;
        s_pending_prefetch_idx = -1;
        s_prefetch_track_idx = -1;
        if (next_idx < s_num_tracks) {
            char next_url[PLEX_MAX_URL];
            if (plex_api_get_stream_url(&s_tracks[next_idx], next_url, sizeof(next_url))) {
                audio_player_prefetch_url(next_url);
                s_prefetch_track_idx = next_idx;
            }
        }
    }

    // Adaptive quality: handle quality downgrade requests
    if (pstate == PLAYER_PLAYING && audio_player_needs_quality_downgrade()) {
        audio_player_clear_downgrade_flag();
        
        if (plex_api_quality_step_down()) {
            PlexQualityTier new_tier = plex_api_get_quality_tier();
            LOG_WARN("Adaptive quality: stepping down to %s for track %d",
                     plex_api_get_quality_label(new_tier), s_current_track_idx + 1);
            snprintf(s_status_msg, sizeof(s_status_msg), "Quality: %s (auto)", 
                     plex_api_get_quality_label(new_tier));
            s_status_color = COL_WARN;
            
            // Restart current track at new quality
            if (s_current_track_idx >= 0 && s_current_track_idx < s_num_tracks) {
                play_track(s_current_track_idx);
            }
        } else {
            audio_player_clear_downgrade_flag();
            LOG_WARN("Already at lowest quality tier - cannot downgrade further");
        }
    }
    
    if (pstate == PLAYER_ERROR && s_current_track_idx >= 0) {
        LOG_WARN("Track %d (%s) encountered stream error (HTTP 400). Skipping...",
            s_current_track_idx + 1, s_tracks[s_current_track_idx].title);
        snprintf(s_status_msg, sizeof(s_status_msg), "Track %d failed (HTTP 400). Skipping...", s_current_track_idx + 1);
        s_status_color = COL_ERROR;

        plex_api_report_timeline_async(s_tracks[s_current_track_idx].rating_key, "stopped", audio_player_get_position_ms(), s_tracks[s_current_track_idx].duration);

        if (!s_auto_advance || !play_next_track()) {
            s_auto_advance = false;
            s_current_track_idx = -1;
            audio_player_stop();
        }
    } else if (pstate == PLAYER_STOPPED && s_current_track_idx >= 0 && s_auto_advance) {
        if (audio_player_get_position_ms() > 1000) {
            plex_api_report_timeline_async(s_tracks[s_current_track_idx].rating_key, "stopped", s_tracks[s_current_track_idx].duration, s_tracks[s_current_track_idx].duration);
            if (!play_next_track()) {
                s_auto_advance = false;
                s_current_track_idx = -1;
            }
        } else {
            s_auto_advance = false;
            s_current_track_idx = -1;
        }
    }

    if (kDown & KEY_SELECT) {
        if (s_screen == SCREEN_NOW_PLAYING) {
            ui_set_screen(s_prev_screen);
        } else {
            s_prev_screen = s_screen;
            ui_set_screen(SCREEN_NOW_PLAYING);
        }
    }

    // New 3DS ZL / ZR Track Navigation Triggers
    if (kDown & KEY_ZR) {
        play_next_track();
    }
    if (kDown & KEY_ZL) {
        play_prev_track();
    }

    // L/R alone cycle the top-screen view (Now Playing / Lyrics / Visualizer).
    // Guarded against the other shoulder button being held so this doesn't
    // also fire on the L+R combo used above to toggle the log viewer.
    if ((kDown & KEY_R) && !(kHeld & KEY_L)) {
        s_top_view = (TopView)((s_top_view + 1) % TOPVIEW_COUNT);
    }
    if ((kDown & KEY_L) && !(kHeld & KEY_R)) {
        s_top_view = (TopView)((s_top_view + TOPVIEW_COUNT - 1) % TOPVIEW_COUNT);
    }
    // X cycles which visualizer style is shown, only meaningful in that view.
    if ((kDown & KEY_X) && s_top_view == TOPVIEW_VISUALIZER) {
        s_vis_style = (VisStyle)((s_vis_style + 1) % VIS_STYLE_COUNT);
    }

    // New 3DS C-Stick Navigation
    if (kDown & KEY_CSTICK_UP) {
        if (s_selected_idx > 0) {
            s_selected_idx--;
            if (s_selected_idx < s_list_offset) s_list_offset = s_selected_idx;
        }
    }
    if (kDown & KEY_CSTICK_DOWN) {
        if (s_selected_idx < s_list_count - 1) {
            s_selected_idx++;
            if (s_selected_idx >= s_list_offset + LIST_VISIBLE_ITEMS) s_list_offset = s_selected_idx - LIST_VISIBLE_ITEMS + 1;
        }
    }
    // New 3DS C-Stick shortcuts for shuffle/repeat (also reachable via touch
    // on the Now Playing Controls screen - see its input handler below).
    if (kDown & KEY_CSTICK_LEFT) {
        s_shuffle_enabled = !s_shuffle_enabled;
        s_shuffle_history_count = 0; // stale once shuffle state changes
    }
    if (kDown & KEY_CSTICK_RIGHT) {
        s_repeat_mode = (RepeatMode)((s_repeat_mode + 1) % REPEAT_MODE_COUNT);
    }
    if (kDown & KEY_Y) {
        audio_player_toggle();
        PlayerState new_st = audio_player_get_state();
        if (s_current_track_idx >= 0 && s_current_track_idx < s_num_tracks) {
            plex_api_report_timeline_async(s_tracks[s_current_track_idx].rating_key, new_st == PLAYER_PAUSED ? "paused" : "playing", audio_player_get_position_ms(), s_tracks[s_current_track_idx].duration);
        }
        if (new_st == PLAYER_STOPPED) {
            s_auto_advance = false;
        } else {
            s_auto_advance = true;
        }
    }

    if (s_screen == SCREEN_LIBRARIES && s_need_load_libraries) {
        s_need_load_libraries = false;
        s_num_libraries = plex_api_get_music_libraries(s_libraries, PLEX_MAX_ITEMS);
        s_list_count = s_num_libraries;
        return;
    }

    // Now Playing Controls Screen Input Handler
    if (s_screen == SCREEN_NOW_PLAYING) {
        if (kDown & KEY_B) {
            ui_set_screen(SCREEN_HUB);
            return;
        }
        if ((kDown & KEY_TOUCH) && touch.px > 0 && touch.py > 0) {
            if (s_current_track_idx >= 0 && s_current_track_idx < s_num_tracks &&
                touch.px >= 20 && touch.px <= BTM_WIDTH - 20 && touch.py >= 78 && touch.py <= 96) {
                // Tap on the seek bar - jump to that position in the track.
                float frac = (float)(touch.px - 20) / (float)(BTM_WIDTH - 40);
                if (frac < 0.0f) frac = 0.0f;
                if (frac > 1.0f) frac = 1.0f;
                seek_to((int)(frac * s_tracks[s_current_track_idx].duration));
            }
            else if (touch.px >= 15 && touch.px <= 60 && touch.py >= 108 && touch.py <= 153) {
                s_shuffle_enabled = !s_shuffle_enabled;
                s_shuffle_history_count = 0;
            }
            else if (touch.px >= 65 && touch.px <= 120 && touch.py >= 108 && touch.py <= 153) {
                play_prev_track();
            }
            else if (touch.px >= 125 && touch.px <= 195 && touch.py >= 103 && touch.py <= 158) {
                audio_player_toggle();
            }
            else if (touch.px >= 200 && touch.px <= 255 && touch.py >= 108 && touch.py <= 153) {
                play_next_track();
            }
            else if (touch.px >= 260 && touch.px <= 305 && touch.py >= 108 && touch.py <= 153) {
                s_repeat_mode = (RepeatMode)((s_repeat_mode + 1) % REPEAT_MODE_COUNT);
            }
            else if (touch.px >= 20 && touch.px <= 155 && touch.py >= 168 && touch.py <= 213) {
                ui_set_screen(SCREEN_QUEUE);
            }
            else if (touch.px >= 165 && touch.px <= 300 && touch.py >= 168 && touch.py <= 213) {
                ui_set_screen(SCREEN_LYRICS);
            }
        }
        return;
    }

    // Play Queue Screen Input Handler
    if (s_screen == SCREEN_QUEUE) {
        if (kDown & KEY_B) {
            ui_set_screen(SCREEN_NOW_PLAYING);
            return;
        }
        if (kDown & KEY_UP) {
            if (s_selected_idx > 0) {
                s_selected_idx--;
                if (s_selected_idx < s_list_offset) s_list_offset = s_selected_idx;
            }
        }
        if (kDown & KEY_DOWN) {
            if (s_selected_idx < s_list_count - 1) {
                s_selected_idx++;
                if (s_selected_idx >= s_list_offset + LIST_VISIBLE_ITEMS) s_list_offset = s_selected_idx - LIST_VISIBLE_ITEMS + 1;
            }
        }
        if (kDown & KEY_TOUCH && touch.px > 0 && touch.py > LIST_START_Y) {
            int idx = (touch.py - LIST_START_Y) / LIST_ITEM_HEIGHT + s_list_offset;
            if (idx >= 0 && idx < s_list_count) {
                s_selected_idx = idx;
                play_track(idx);
            }
        }
        if (kDown & KEY_A && s_selected_idx >= 0 && s_selected_idx < s_list_count) {
            play_track(s_selected_idx);
        }
        return;
    }

    // Synced Lyrics Screen Input Handler
    if (s_screen == SCREEN_LYRICS) {
        if (kDown & KEY_B) {
            ui_set_screen(SCREEN_NOW_PLAYING);
            return;
        }
        return;
    }

    // 1. PIN Linking Screen (Auto-polling)
    if (s_screen == SCREEN_LINK_PIN) {
        if (kDown & KEY_B) {
            ui_set_screen(SCREEN_AUTH_CHOICE);
            return;
        }
        s_pin_timer++;
        if (s_pin_timer % 90 == 0 && s_pin.code[0] != '\0') {
            if (plex_api_check_pin(&s_pin)) {
                strncpy(s_config->auth_token, s_pin.auth_token, sizeof(s_config->auth_token) - 1);
                config_save(s_config);
                s_num_servers = 0; // force refresh
                ui_set_screen(SCREEN_SERVER_SELECT);
                return;
            } else if (s_pin.expired) {
                snprintf(s_status_msg, sizeof(s_status_msg), "PIN expired. Press A to retry.");
                s_status_color = COL_ERROR;
            }
        }
        if ((kDown & KEY_A) && s_pin.expired) {
            ui_set_screen(SCREEN_LINK_PIN);
        }
        return;
    }

    // 2. Auth Choice Screen
    if (s_screen == SCREEN_AUTH_CHOICE) {
        if (kDown & KEY_UP) s_selected_idx = (s_selected_idx > 0) ? s_selected_idx - 1 : 2;
        if (kDown & KEY_DOWN) s_selected_idx = (s_selected_idx < 2) ? s_selected_idx + 1 : 0;
        
        if (kDown & KEY_TOUCH && touch.px > 0 && touch.py > LIST_START_Y) {
            int idx = (touch.py - LIST_START_Y) / LIST_ITEM_HEIGHT;
            if (idx >= 0 && idx < 3) {
                s_selected_idx = idx;
                kDown |= KEY_A;
            }
        }
        
        if (kDown & KEY_A) {
            if (s_selected_idx == 0) ui_set_screen(SCREEN_LINK_PIN);
            else if (s_selected_idx == 1) ui_set_screen(SCREEN_LOGIN_DIRECT);
            else if (s_selected_idx == 2) ui_set_screen(SCREEN_SETUP);
        }
        return;
    }

    // 3. Direct Login (Username + Password + 2FA) Screen
    if (s_screen == SCREEN_LOGIN_DIRECT) {
        if (kDown & KEY_B) {
            ui_set_screen(SCREEN_AUTH_CHOICE);
            return;
        }
        if (kDown & KEY_UP) s_setup_field = (s_setup_field > 0) ? s_setup_field - 1 : 3;
        if (kDown & KEY_DOWN) s_setup_field = (s_setup_field < 3) ? s_setup_field + 1 : 0;
        
        if (kDown & KEY_TOUCH && touch.px > 0 && touch.py > 0) {
            if (touch.py >= 45 && touch.py < 45 + LIST_ITEM_HEIGHT) s_setup_field = 0;
            else if (touch.py >= 45 + LIST_ITEM_HEIGHT && touch.py < 45 + 2*LIST_ITEM_HEIGHT) s_setup_field = 1;
            else if (touch.py >= 45 + 2*LIST_ITEM_HEIGHT && touch.py < 45 + 3*LIST_ITEM_HEIGHT) s_setup_field = 2;
            else if (touch.py >= 45 + 3*LIST_ITEM_HEIGHT && touch.py < 45 + 4*LIST_ITEM_HEIGHT) s_setup_field = 3;
        }
        
        if ((kDown & KEY_A) || (kDown & KEY_TOUCH)) {
            if (s_setup_field == 0) {
                show_keyboard("Enter Email / Username", s_login_user, sizeof(s_login_user));
            } else if (s_setup_field == 1) {
                show_keyboard("Enter Password", s_login_pass, sizeof(s_login_pass));
            } else if (s_setup_field == 2) {
                show_keyboard("Enter 2FA Code (Optional)", s_login_2fa, sizeof(s_login_2fa));
            } else if (s_setup_field == 3) {
                snprintf(s_status_msg, sizeof(s_status_msg), "Signing in...");
                s_status_color = COL_TEXT;
                char token[128] = "";
                int res = plex_api_login_direct(s_login_user, s_login_pass, s_login_2fa, token, sizeof(token));
                if (res == 1) {
                    strncpy(s_config->auth_token, token, sizeof(s_config->auth_token) - 1);
                    config_save(s_config);
                    s_num_servers = 0;
                    ui_set_screen(SCREEN_SERVER_SELECT);
                } else if (res == 2) {
                    snprintf(s_status_msg, sizeof(s_status_msg), "Login failed! Enter 2FA code if enabled.");
                    s_status_color = COL_ERROR;
                } else {
                    snprintf(s_status_msg, sizeof(s_status_msg), "Network or connection error!");
                    s_status_color = COL_ERROR;
                }
            }
        }
        return;
    }

    // 4. Server Selection Screen
    if (s_screen == SCREEN_SERVER_SELECT) {
        if (kDown & KEY_B) {
            ui_set_screen(SCREEN_AUTH_CHOICE);
            return;
        }
        if (kDown & KEY_UP) {
            if (s_selected_idx > 0) {
                s_selected_idx--;
                if (s_selected_idx < s_list_offset) s_list_offset = s_selected_idx;
            }
        }
        if (kDown & KEY_DOWN) {
            if (s_selected_idx < s_list_count - 1) {
                s_selected_idx++;
                if (s_selected_idx >= s_list_offset + LIST_VISIBLE_ITEMS) s_list_offset = s_selected_idx - LIST_VISIBLE_ITEMS + 1;
            }
        }
        if (kDown & KEY_TOUCH && touch.px > 0 && touch.py > LIST_START_Y) {
            int idx = (touch.py - LIST_START_Y) / LIST_ITEM_HEIGHT + s_list_offset;
            if (idx >= 0 && idx < s_list_count) {
                s_selected_idx = idx;
                kDown |= KEY_A;
            }
        }
        if (kDown & KEY_A && s_num_servers > 0) {
            strncpy(s_config->server_url, s_servers[s_selected_idx].uri, sizeof(s_config->server_url) - 1);
            strncpy(s_config->auth_token, s_servers[s_selected_idx].access_token, sizeof(s_config->auth_token) - 1);
            config_save(s_config);
            plex_api_init(s_config->server_url, s_config->auth_token);
            ui_set_screen(SCREEN_HUB);
        }
        return;
    }

    // 5. Main Hub Screen
    if (s_screen == SCREEN_HUB) {
        if (kDown & KEY_UP) s_selected_idx = (s_selected_idx > 0) ? s_selected_idx - 1 : 4;
        if (kDown & KEY_DOWN) s_selected_idx = (s_selected_idx < 4) ? s_selected_idx + 1 : 0;
        
        if (kDown & KEY_TOUCH && touch.px > 0 && touch.py > LIST_START_Y) {
            int idx = (touch.py - LIST_START_Y) / LIST_ITEM_HEIGHT;
            if (idx >= 0 && idx < 5) {
                s_selected_idx = idx;
                kDown |= KEY_A;
            }
        }
        
        if (kDown & KEY_A) {
            if (s_selected_idx == 0) { // Artists
                if (s_num_libraries == 0) s_num_libraries = plex_api_get_music_libraries(s_libraries, PLEX_MAX_ITEMS);
                if (s_num_libraries > 0) {
                    strncpy(s_current_title, s_libraries[0].title, PLEX_MAX_STR);
                    s_num_artists = plex_api_get_artists(s_libraries[0].key, s_artists, PLEX_MAX_ITEMS);
                    ui_set_screen(SCREEN_ARTISTS);
                } else {
                    ui_set_screen(SCREEN_LIBRARIES);
                }
            } else if (s_selected_idx == 1) { // Playlists
                ui_set_screen(SCREEN_PLAYLISTS);
            } else if (s_selected_idx == 2) { // Search
                char query[128] = "";
                if (show_keyboard("Search Music Tracks", query, sizeof(query)) && query[0] != '\0') {
                    strncpy(s_current_title, "Search Results", PLEX_MAX_STR);
                    s_num_tracks = plex_api_search_tracks(query, s_tracks, PLEX_MAX_ITEMS);
                    ui_set_screen(SCREEN_TRACKS);
                }
            } else if (s_selected_idx == 3) { // Recently Added
                strncpy(s_current_title, "Recently Added Tracks", PLEX_MAX_STR);
                s_num_tracks = plex_api_get_recently_added(s_tracks, PLEX_MAX_ITEMS);
                ui_set_screen(SCREEN_TRACKS);
            } else if (s_selected_idx == 4) { // All Libraries
                ui_set_screen(SCREEN_LIBRARIES);
            }
        }
        return;
    }

    // 6. Manual Setup Screen
    if (s_screen == SCREEN_SETUP) {
        if (kDown & KEY_B) {
            ui_set_screen(SCREEN_AUTH_CHOICE);
            return;
        }
        if (kDown & KEY_UP) s_setup_field = (s_setup_field > 0) ? s_setup_field - 1 : 2;
        if (kDown & KEY_DOWN) s_setup_field = (s_setup_field < 2) ? s_setup_field + 1 : 0;
        
        if (kDown & KEY_TOUCH && touch.px > 0 && touch.py > 0) {
            if (touch.py >= 50 && touch.py < 50 + LIST_ITEM_HEIGHT) s_setup_field = 0;
            else if (touch.py >= 50 + LIST_ITEM_HEIGHT && touch.py < 50 + 2*LIST_ITEM_HEIGHT) s_setup_field = 1;
            else if (touch.py >= 50 + 2*LIST_ITEM_HEIGHT && touch.py < 50 + 3*LIST_ITEM_HEIGHT) s_setup_field = 2;
        }
        
        if (kDown & KEY_A || (kDown & KEY_TOUCH)) {
            if (s_setup_field == 0) {
                if (show_keyboard("Enter Server URL", s_config->server_url, sizeof(s_config->server_url))) {
                    config_save(s_config);
                }
            } else if (s_setup_field == 1) {
                if (show_keyboard("Enter Auth Token", s_config->auth_token, sizeof(s_config->auth_token))) {
                    config_save(s_config);
                }
            } else if (s_setup_field == 2) {
                plex_api_init(s_config->server_url, s_config->auth_token);
                if (plex_api_test_connection()) {
                    ui_set_screen(SCREEN_HUB);
                    snprintf(s_status_msg, sizeof(s_status_msg), "Connected successfully!");
                    s_status_color = COL_PLAYING;
                } else {
                    snprintf(s_status_msg, sizeof(s_status_msg), "Connection failed!");
                    s_status_color = COL_ERROR;
                }
            }
        }
        return;
    }

    if (kDown & KEY_UP) {
        if (s_selected_idx > 0) {
            s_selected_idx--;
            if (s_selected_idx < s_list_offset) s_list_offset = s_selected_idx;
        }
    }
    if (kDown & KEY_DOWN) {
        if (s_selected_idx < s_list_count - 1) {
            s_selected_idx++;
            if (s_selected_idx >= s_list_offset + LIST_VISIBLE_ITEMS) s_list_offset = s_selected_idx - LIST_VISIBLE_ITEMS + 1;
        }
    }
    
    if (kDown & KEY_TOUCH && touch.px > 0 && touch.py > LIST_START_Y) {
        int idx = (touch.py - LIST_START_Y) / LIST_ITEM_HEIGHT + s_list_offset;
        if (idx >= 0 && idx < s_list_count) {
            s_selected_idx = idx;
            kDown |= KEY_A;
        }
    }

    if (kDown & KEY_B) {
        if (s_screen == SCREEN_ARTISTS) ui_set_screen(SCREEN_HUB);
        else if (s_screen == SCREEN_PLAYLISTS) ui_set_screen(SCREEN_HUB);
        else if (s_screen == SCREEN_LIBRARIES) ui_set_screen(SCREEN_HUB);
        else if (s_screen == SCREEN_HUB) ui_set_screen(SCREEN_AUTH_CHOICE);
        else if (s_screen == SCREEN_ALBUMS) ui_set_screen(SCREEN_ARTISTS);
        else if (s_screen == SCREEN_TRACKS) ui_set_screen(SCREEN_HUB);
    }

    if (kDown & KEY_A) {
        if (s_screen == SCREEN_LIBRARIES && s_list_count > 0) {
            strncpy(s_current_title, s_libraries[s_selected_idx].title, PLEX_MAX_STR);
            strncpy(s_active_key, s_libraries[s_selected_idx].key, PLEX_MAX_URL);
            s_loaded_items = plex_api_get_artists_page(s_active_key, s_artists, 0, PLEX_PAGE_SIZE, &s_total_items);
            s_num_artists = s_loaded_items;
            ui_set_screen(SCREEN_ARTISTS);
        } else if (s_screen == SCREEN_ARTISTS && s_list_count > 0 && s_selected_idx < s_num_artists) {
            strncpy(s_current_title, s_artists[s_selected_idx].title, PLEX_MAX_STR);
            strncpy(s_active_key, s_artists[s_selected_idx].key, PLEX_MAX_URL);
            s_loaded_items = plex_api_get_albums_page(s_active_key, s_albums, 0, PLEX_PAGE_SIZE, &s_total_items);
            s_num_albums = s_loaded_items;
            ui_set_screen(SCREEN_ALBUMS);
        } else if (s_screen == SCREEN_ALBUMS && s_list_count > 0 && s_selected_idx < s_num_albums) {
            strncpy(s_current_title, s_albums[s_selected_idx].title, PLEX_MAX_STR);
            strncpy(s_active_key, s_albums[s_selected_idx].key, PLEX_MAX_URL);
            s_loaded_items = plex_api_get_tracks_page(s_active_key, s_tracks, 0, PLEX_PAGE_SIZE, &s_total_items);
            s_num_tracks = s_loaded_items;
            ui_set_screen(SCREEN_TRACKS);
        } else if (s_screen == SCREEN_PLAYLISTS && s_list_count > 0) {
            strncpy(s_current_title, s_playlists[s_selected_idx].title, PLEX_MAX_STR);
            strncpy(s_active_key, s_playlists[s_selected_idx].key, PLEX_MAX_URL);
            s_loaded_items = plex_api_get_tracks_page(s_active_key, s_tracks, 0, PLEX_PAGE_SIZE, &s_total_items);
            s_num_tracks = s_loaded_items;
            ui_set_screen(SCREEN_TRACKS);
        } else if (s_screen == SCREEN_TRACKS && s_list_count > 0 && s_selected_idx < s_num_tracks) {
            play_track(s_selected_idx);
        }
    }

    // Continuous Scrolling Lazy Loading Trigger
    if ((s_screen == SCREEN_ARTISTS || s_screen == SCREEN_ALBUMS || s_screen == SCREEN_TRACKS) && !s_is_lazy_loading) {
        int current_count = (s_screen == SCREEN_ARTISTS) ? s_num_artists : ((s_screen == SCREEN_ALBUMS) ? s_num_albums : s_num_tracks);
        if (current_count < s_total_items && s_selected_idx >= current_count - 5) {
            s_is_lazy_loading = true;
            int new_items = 0;
            if (s_screen == SCREEN_ARTISTS) {
                new_items = plex_api_get_artists_page(s_active_key, s_artists, current_count, PLEX_PAGE_SIZE, NULL);
                s_num_artists += new_items;
            } else if (s_screen == SCREEN_ALBUMS) {
                new_items = plex_api_get_albums_page(s_active_key, s_albums, current_count, PLEX_PAGE_SIZE, NULL);
                s_num_albums += new_items;
            } else if (s_screen == SCREEN_TRACKS) {
                new_items = plex_api_get_tracks_page(s_active_key, s_tracks, current_count, PLEX_PAGE_SIZE, NULL);
                s_num_tracks += new_items;
            }
            s_loaded_items = (s_screen == SCREEN_ARTISTS) ? s_num_artists : ((s_screen == SCREEN_ALBUMS) ? s_num_albums : s_num_tracks);
            s_is_lazy_loading = false;
        }
    }
}

// --- Visualizers: driven by audio_player_get_visualizer_samples() ------

#define VIS_MAX_SAMPLES 256

// VU-style bars: splits the latest decoded samples into buckets, plots each
// bucket's RMS amplitude as a bar. Bars rise instantly to a loud moment and
// decay slowly afterward, for the usual "bouncing levels" look.
static void draw_visualizer_bars(float x, float y, float w, float h) {
    s16 samples[VIS_MAX_SAMPLES];
    int n = audio_player_get_visualizer_samples(samples, VIS_MAX_SAMPLES);

    #define VIS_NUM_BARS 24
    static float s_bar_level[VIS_NUM_BARS] = {0};

    float bar_w = w / VIS_NUM_BARS;
    float baseline = y + h;

    for (int b = 0; b < VIS_NUM_BARS; b++) {
        float target = 0.0f;
        if (n > 1) {
            int start = (b * n) / VIS_NUM_BARS;
            int end = ((b + 1) * n) / VIS_NUM_BARS;
            if (end <= start) end = start + 1;
            if (end > n) end = n;

            double sum_sq = 0.0;
            int count = 0;
            for (int i = start; i < end; i++) {
                double v = samples[i];
                sum_sq += v * v;
                count++;
            }
            if (count > 0) {
                float rms = (float)sqrt(sum_sq / count);
                target = rms / 32768.0f;
                if (target > 1.0f) target = 1.0f;
            }
        }

        if (target > s_bar_level[b]) {
            s_bar_level[b] = target;       // rise instantly
        } else {
            s_bar_level[b] *= 0.85f;       // decay each frame
        }

        float bh = s_bar_level[b] * h;
        if (bh < 3.0f) bh = 3.0f;          // always show a sliver so it doesn't look dead

        float bx = x + b * bar_w;
        C2D_DrawRectSolid(bx + 1, baseline - bh, 0.5f, bar_w - 2, bh, COL_ACCENT);
    }
}

// Oscilloscope: plots the latest decoded samples as a connected waveform.
static void draw_visualizer_oscilloscope(float x, float y, float w, float h) {
    s16 samples[VIS_MAX_SAMPLES];
    int n = audio_player_get_visualizer_samples(samples, VIS_MAX_SAMPLES);
    if (n < 2) return;

    float mid_y = y + h / 2.0f;
    float half_h = h / 2.0f;

    float prev_x = x;
    float prev_y = mid_y - ((float)samples[0] / 32768.0f) * half_h;

    for (int i = 1; i < n; i++) {
        float px = x + ((float)i / (float)(n - 1)) * w;
        float py = mid_y - ((float)samples[i] / 32768.0f) * half_h;
        C2D_DrawLine(prev_x, prev_y, COL_ACCENT, px, py, COL_ACCENT, 2.0f, 0.5f);
        prev_x = px;
        prev_y = py;
    }
}

static void draw_visualizer(VisStyle style, float x, float y, float w, float h) {
    C2D_DrawRectSolid(x, y, 0.4f, w, h, RGBA8(0x0C, 0x0C, 0x18, 0xFF));
    switch (style) {
        case VIS_STYLE_OSCILLOSCOPE: draw_visualizer_oscilloscope(x, y, w, h); break;
        case VIS_STYLE_BARS:
        default:                     draw_visualizer_bars(x, y, w, h); break;
    }
}

// --- Top-screen views: Now Playing / Lyrics / Visualizer ---------------

static void draw_top_now_playing(PlexTrack* track, PlayerState state) {
    // Left Side: Album Art Frame (110x110)
    float art_x = 20;
    float art_y = 45;
    float art_size = 110;

    C2D_DrawRectSolid(art_x - 2, art_y - 2, 0.5f, art_size + 4, art_size + 4, COL_ACCENT);
    C2D_DrawRectSolid(art_x, art_y, 0.5f, art_size, art_size, COL_SURFACE);

    if (album_art_has_texture()) {
        album_art_draw(art_x, art_y, art_size, art_size);
    } else {
        C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 42, C2D_Color32(0x28, 0x28, 0x38, 0xFF));
        C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 34, C2D_Color32(0x18, 0x18, 0x24, 0xFF));

        if (album_art_is_loading()) {
            draw_loading_spinner(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 14, NULL);
        } else {
            C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 14, COL_ACCENT);
            C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 4, C2D_Color32(0x12, 0x12, 0x24, 0xFF));
        }
    }

    // Buffering badge: separate from the album-art spinner above (that one's
    // for the *image* download) - this one means the audio stream itself
    // doesn't have enough data yet to play.
    if (state == PLAYER_LOADING) {
        draw_loading_spinner(art_x + art_size - 12, art_y + 12, 11, NULL);
    }

    // Right Side: Track Details with Marquee Scrolling
    float text_x = 145;
    float max_text_w = TOP_WIDTH - text_x - 15;

    draw_scrolling_text(track->title, text_x, 48, max_text_w, 0.65f, 0.65f, COL_TEXT);
    draw_scrolling_text(track->grandparent_title[0] ? track->grandparent_title : "Unknown Artist", text_x, 76, max_text_w, 0.55f, 0.55f, COL_ACCENT);
    draw_scrolling_text(track->parent_title[0] ? track->parent_title : "", text_x, 98, max_text_w, 0.45f, 0.45f, COL_TEXT_DIM);

    char status_line[128];
    const char* state_str = (state == PLAYER_PLAYING) ? "Playing" : (state == PLAYER_PAUSED ? "Paused" : "Buffering...");
    u32 state_col = (state == PLAYER_LOADING) ? COL_WARN : COL_TEXT_DARK;
    snprintf(status_line, sizeof(status_line), "%s  |  Vol: %d%%", state_str, get_hw_volume_percent());
    draw_text(status_line, text_x, 122, 0.45f, 0.45f, state_col);

    // The seekable progress bar lives on the bottom screen's Now Playing
    // Controls now (touch can't reach the top screen) - see ui_render_bottom().
    char status_line2[128];
    snprintf(status_line2, sizeof(status_line2), "%s  |  Vol: %d%%", state_str, get_hw_volume_percent());
    draw_text_centered(status_line2, 190, TOP_WIDTH, 0.55f, 0.55f, (state == PLAYER_LOADING) ? COL_WARN : COL_TEXT);
}

// Compact album art + details on the left, big time-synced lyrics on the right.
static void draw_top_lyrics(PlexTrack* track, PlayerState state) {
    float art_x = 15;
    float art_y = 42;
    float art_size = 90;

    C2D_DrawRectSolid(art_x - 2, art_y - 2, 0.5f, art_size + 4, art_size + 4, COL_ACCENT);
    C2D_DrawRectSolid(art_x, art_y, 0.5f, art_size, art_size, COL_SURFACE);

    if (album_art_has_texture()) {
        album_art_draw(art_x, art_y, art_size, art_size);
    } else {
        C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 30, C2D_Color32(0x28, 0x28, 0x38, 0xFF));
        C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 22, C2D_Color32(0x18, 0x18, 0x24, 0xFF));
    }

    float details_x = 15;
    float details_w = art_size;
    float details_y = art_y + art_size + 10;
    draw_scrolling_text(track->title, details_x, details_y, details_w, 0.4f, 0.4f, COL_TEXT);
    draw_scrolling_text(track->grandparent_title[0] ? track->grandparent_title : "Unknown Artist",
                         details_x, details_y + 18, details_w, 0.35f, 0.35f, COL_ACCENT);

    int pos_ms = audio_player_get_position_ms();
    char tbuf1[32], tbuf2[32];
    format_time(pos_ms, tbuf1, sizeof(tbuf1));
    format_time(track->duration, tbuf2, sizeof(tbuf2));
    char time_str[80];
    snprintf(time_str, sizeof(time_str), "%s / %s", tbuf1, tbuf2);
    draw_text(time_str, details_x, details_y + 40, 0.32f, 0.32f, COL_TEXT_DIM);

    if (state == PLAYER_LOADING) {
        draw_text("Buffering...", details_x, details_y + 56, 0.3f, 0.3f, COL_WARN);
    }

    // Right column: big lyrics, several lines centered on the current one.
    float lyr_x = 150;
    float lyr_w = TOP_WIDTH - lyr_x - 15;

    if (s_num_lyrics == 0) {
        draw_text_centered_at("Loading lyrics...", lyr_x, 120, lyr_w, 0.45f, 0.45f, COL_TEXT_DIM);
        return;
    }

    // base_y=42, half_window=3, row_h=26 -> spans 42 to 198, with margin
    // clear of both the divider above and the "L/R: Change View" hint below.
    draw_lyrics_block(pos_ms, lyr_x, 42, lyr_w, 26.0f, 3, 0.55f, 0.4f, COL_ACCENT, COL_TEXT_DIM);
}

// Track details in small text at top, visualizer filling the rest.
static void draw_top_visualizer(PlexTrack* track, PlayerState state) {
    char info_line[128];
    const char* state_str = (state == PLAYER_PLAYING) ? "Playing" : (state == PLAYER_PAUSED ? "Paused" : "Buffering...");
    snprintf(info_line, sizeof(info_line), "%s - %s  |  %s",
             track->title, track->grandparent_title[0] ? track->grandparent_title : "Unknown Artist", state_str);
    draw_text_centered(info_line, 40, TOP_WIDTH, 0.45f, 0.45f, state == PLAYER_LOADING ? COL_WARN : COL_TEXT);

    const char* style_name = (s_vis_style == VIS_STYLE_OSCILLOSCOPE) ? "Oscilloscope" : "VU Bars";
    char style_line[64];
    snprintf(style_line, sizeof(style_line), "%s  (X: change style)", style_name);
    draw_text_centered(style_line, 58, TOP_WIDTH, 0.35f, 0.35f, COL_TEXT_DIM);

    if (state == PLAYER_LOADING) {
        // Nothing's decoded yet to visualize - say so instead of drawing a dead/flat plot.
        C2D_DrawRectSolid(15, 78, 0.4f, TOP_WIDTH - 30, 130, RGBA8(0x0C, 0x0C, 0x18, 0xFF));
        draw_loading_spinner(TOP_WIDTH / 2.0f, 78 + 65, 20, NULL);
    } else {
        draw_visualizer(s_vis_style, 15, 78, TOP_WIDTH - 30, 130);
    }
}

void ui_render_top(C3D_RenderTarget* top) {
    C2D_TextBufClear(s_text_buf);

    draw_text_centered("DUALPLEX", 10, TOP_WIDTH, 0.6f, 0.6f, COL_ACCENT);
    C2D_DrawRectSolid(10, 30, 0.5f, TOP_WIDTH - 20, 2, COL_ACCENT);

    // Top-Left HUD: WiFi Signal Indicator
    draw_wifi_indicator(10, 23);

    // Top-Right HUD: Time & Battery Indicator
    time_t rawtime = time(NULL);
    struct tm* timeinfo = localtime(&rawtime);
    char hud_str[64];
    if (timeinfo) {
        u8 bat_percent = 100;
        u8 bat_stat = 5;
        if (R_SUCCEEDED(PTMU_GetBatteryLevel(&bat_stat))) {
            bat_percent = (bat_stat >= 5) ? 100 : (bat_stat * 20);
        }
        u8 is_charging = 0;
        PTMU_GetBatteryChargeState(&is_charging);
        
        char t_str[16];
        strftime(t_str, sizeof(t_str), "%I:%M %p", timeinfo);
        snprintf(hud_str, sizeof(hud_str), "%s | %s%d%%", t_str, is_charging ? "\xE2\x9A\xA1 " : "", bat_percent);
    } else {
        snprintf(hud_str, sizeof(hud_str), "12:00 PM | 100%%");
    }
    draw_text(hud_str, TOP_WIDTH - 130, 10, 0.42f, 0.42f, COL_TEXT_DIM);

    if (s_screen == SCREEN_LINK_PIN) {
        draw_text_centered("LINK DEVICE WITH PLEX", 45, TOP_WIDTH, 0.65f, 0.65f, COL_TEXT);
        draw_text_centered("1. Visit: https://plex.tv/link", 75, TOP_WIDTH, 0.55f, 0.55f, COL_TEXT_DIM);
        draw_text_centered("2. Enter Code on your phone/PC:", 100, TOP_WIDTH, 0.55f, 0.55f, COL_TEXT_DIM);
        
        char code_box[32];
        if (s_pin.code[0]) {
            snprintf(code_box, sizeof(code_box), "[  %s  ]", s_pin.code);
        } else {
            snprintf(code_box, sizeof(code_box), "[  ....  ]");
        }
        draw_text_centered(code_box, 135, TOP_WIDTH, 1.1f, 1.1f, COL_ACCENT);
        
        draw_text_centered(s_status_msg, 195, TOP_WIDTH, 0.5f, 0.5f, s_status_color);
        return;
    }

    PlayerState state = audio_player_get_state();
    
    if (state == PLAYER_PLAYING || state == PLAYER_PAUSED || state == PLAYER_LOADING) {
        if (s_current_track_idx >= 0 && s_current_track_idx < s_num_tracks) {
            PlexTrack* track = &s_tracks[s_current_track_idx];
            switch (s_top_view) {
                case TOPVIEW_LYRICS:     draw_top_lyrics(track, state); break;
                case TOPVIEW_VISUALIZER: draw_top_visualizer(track, state); break;
                case TOPVIEW_NOW_PLAYING:
                default:                 draw_top_now_playing(track, state); break;
            }
        }
    } else {
        float art_x = (TOP_WIDTH - 80) / 2.0f;
        float art_y = 50;
        float art_size = 80;
        
        C2D_DrawRectSolid(art_x - 2, art_y - 2, 0.5f, art_size + 4, art_size + 4, COL_SURFACE);
        C2D_DrawRectSolid(art_x, art_y, 0.5f, art_size, art_size, COL_BG);
        
        C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 32, C2D_Color32(0x28, 0x28, 0x38, 0xFF));
        C2D_DrawCircleSolid(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 0.5f, 24, C2D_Color32(0x18, 0x18, 0x24, 0xFF));
        
        draw_loading_spinner(art_x + art_size / 2.0f, art_y + art_size / 2.0f, 10, NULL);
        
        draw_text_centered("NO TRACK PLAYING", 145, TOP_WIDTH, 0.65f, 0.65f, COL_ACCENT);
        draw_text_centered("Select a track to start playback", 170, TOP_WIDTH, 0.45f, 0.45f, COL_TEXT_DIM);
        
        if (s_status_msg[0]) {
            draw_text_centered(s_status_msg, 195, TOP_WIDTH, 0.48f, 0.48f, s_status_color);
        }
    }
    
    // Bottom of Top Screen: Build Timestamp & Live Log Hint
    draw_text_centered("L/R: Change View  |  L+R Together: Logs", TOP_HEIGHT - 16, TOP_WIDTH, 0.38f, 0.38f, COL_TEXT_DIM);
}

static void draw_log_entry_wrapped(const char* line, float x, float* y, float max_w, u32 color) {
    if (!line || !line[0] || *y > BTM_HEIGHT - 30) return;
    
    char line_copy[256];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';
    
    float scale = 0.32f;
    float line_h = 15.0f;
    
    int len = (int)strlen(line_copy);
    int start = 0;
    
    while (start < len && *y <= BTM_HEIGHT - 30) {
        char chunk[64];
        int chunk_len = 0;
        
        while (start + chunk_len < len && chunk_len < 58) {
            chunk[chunk_len] = line_copy[start + chunk_len];
            chunk_len++;
        }
        chunk[chunk_len] = '\0';
        
        char display_buf[70];
        if (start == 0) {
            snprintf(display_buf, sizeof(display_buf), "%s", chunk);
        } else {
            snprintf(display_buf, sizeof(display_buf), " -> %s", chunk);
        }
        
        draw_text(display_buf, x, *y, scale, scale, color);
        *y += line_h;
        start += chunk_len;
    }
}

void ui_render_bottom(C3D_RenderTarget* bottom) {
    if (s_show_logs) {
        draw_header("Live System Logs");
        int total_logs = logger_get_log_count();
        
        C2D_DrawRectSolid(8, 42, 0.5f, BTM_WIDTH - 16, 168, RGBA8(0x0C, 0x0C, 0x18, 0xFF));
        
        if (total_logs == 0) {
            draw_text_centered("No log entries recorded.", 110, BTM_WIDTH, 0.45f, 0.45f, COL_TEXT_DIM);
        } else {
            float ly = 46;
            for (int i = 0; i < total_logs; i++) {
                int idx = total_logs - 1 - (s_log_scroll_offset + i);
                if (idx < 0 || idx >= total_logs || ly > BTM_HEIGHT - 32) break;
                
                const char* line = logger_get_log_line(idx);
                u32 color = COL_TEXT;
                if (strstr(line, "[ERROR]")) color = COL_ERROR;
                else if (strstr(line, "[WARN]")) color = COL_ACCENT;
                else if (strstr(line, "[INFO]")) color = RGBA8(0x88, 0xDD, 0x88, 0xFF);
                
                draw_log_entry_wrapped(line, 12, &ly, BTM_WIDTH - 24, color);
            }
        }
        
        draw_text_centered("D-Pad: Scroll  |  Press L+R or B to Close", BTM_HEIGHT - 20, BTM_WIDTH, 0.40f, 0.40f, COL_TEXT_DIM);
        return;
    }

    if (s_screen == SCREEN_AUTH_CHOICE) {
        draw_header("Sign In to Plex");
        draw_list_item(0, "1. Link Device (plex.tv/link)", "Recommended - easy auth on phone/PC", s_selected_idx == 0, false);
        draw_list_item(1, "2. Direct Login (User + Pass + 2FA)", "Enter credentials with 3DS keyboard", s_selected_idx == 1, false);
        draw_list_item(2, "3. Manual Server IP & Token", "Direct IP connection", s_selected_idx == 2, false);
        draw_text_centered("Use D-Pad to navigate, A to select", BTM_HEIGHT - 20, BTM_WIDTH, 0.4f, 0.4f, COL_TEXT_DIM);
    } else if (s_screen == SCREEN_LINK_PIN) {
        draw_header("Link Device");
        draw_text_centered("Go to https://plex.tv/link", 60, BTM_WIDTH, 0.55f, 0.55f, COL_TEXT);
        draw_text_centered("and enter the 4-character code", 85, BTM_WIDTH, 0.5f, 0.5f, COL_TEXT_DIM);
        draw_text_centered("shown on the top screen.", 105, BTM_WIDTH, 0.5f, 0.5f, COL_TEXT_DIM);
        draw_text_centered("Press B to Cancel", BTM_HEIGHT - 25, BTM_WIDTH, 0.45f, 0.45f, COL_TEXT_DIM);
    } else if (s_screen == SCREEN_LOGIN_DIRECT) {
        draw_header("Account Login");
        
        char user_disp[128], pass_disp[128], code_disp[128];
        snprintf(user_disp, sizeof(user_disp), "User: %s", s_login_user[0] ? s_login_user : "[tap to set]");
        snprintf(pass_disp, sizeof(pass_disp), "Pass: %s", s_login_pass[0] ? "********" : "[tap to set]");
        snprintf(code_disp, sizeof(code_disp), "2FA Code: %s", s_login_2fa[0] ? s_login_2fa : "[optional]");
        
        draw_list_item(0, user_disp, NULL, s_setup_field == 0, false);
        draw_list_item(1, pass_disp, NULL, s_setup_field == 1, false);
        draw_list_item(2, code_disp, NULL, s_setup_field == 2, false);
        draw_list_item(3, "[ Sign In ]", NULL, s_setup_field == 3, false);
        
        if (s_status_msg[0]) {
            draw_text_centered(s_status_msg, BTM_HEIGHT - 22, BTM_WIDTH, 0.4f, 0.4f, s_status_color);
        }
    } else if (s_screen == SCREEN_SERVER_SELECT) {
        draw_header("Select Plex Server");
        if (s_num_servers == 0) {
            draw_text_centered("Searching for servers...", 100, BTM_WIDTH, 0.5f, 0.5f, COL_TEXT_DIM);
        } else {
            for (int i = 0; i < LIST_VISIBLE_ITEMS; i++) {
                int idx = s_list_offset + i;
                if (idx >= s_num_servers) break;
                char sub[128];
                snprintf(sub, sizeof(sub), "%s (%s)", s_servers[idx].is_local ? "Local Network" : "Remote", s_servers[idx].uri);
                draw_list_item(i, s_servers[idx].name, sub, idx == s_selected_idx, false);
            }
        }
    } else if (s_screen == SCREEN_HUB) {
        draw_header("DualPlex Music Hub");
        draw_list_item(0, "Artists", "Browse all artists", s_selected_idx == 0, false);
        draw_list_item(1, "Playlists", "View music playlists", s_selected_idx == 1, false);
        draw_list_item(2, "Search Library", "Search tracks, artists & albums", s_selected_idx == 2, false);
        draw_list_item(3, "Recently Added", "Stream newly added tracks", s_selected_idx == 3, false);
        draw_list_item(4, "All Libraries", "Select music library section", s_selected_idx == 4, false);
        
        bool is_n3ds = false;
        APT_CheckNew3DS(&is_n3ds);
        if (is_n3ds) {
            draw_text_centered("⚡ 804MHz N3DS Speedup Active (ZL/ZR/C-Stick Controls)", BTM_HEIGHT - 14, BTM_WIDTH, 0.38f, 0.38f, COL_ACCENT);
        }
    } else if (s_screen == SCREEN_SETUP) {
        draw_header("Manual Setup");
        
        char url_disp[128];
        if (s_config->server_url[0]) {
            snprintf(url_disp, sizeof(url_disp), "Server URL: %.32s...", s_config->server_url);
        } else {
            snprintf(url_disp, sizeof(url_disp), "Server URL: [tap to set]");
        }
        
        char auth_disp[128];
        if (s_config->auth_token[0]) {
            snprintf(auth_disp, sizeof(auth_disp), "Auth Token: ****");
        } else {
            snprintf(auth_disp, sizeof(auth_disp), "Auth Token: [tap to set]");
        }
        
        draw_list_item(0, url_disp, NULL, s_setup_field == 0, false);
        draw_list_item(1, auth_disp, NULL, s_setup_field == 1, false);
        draw_list_item(2, "[ Connect ]", NULL, s_setup_field == 2, false);
        
        draw_text_centered("Use D-Pad to navigate, A to edit/connect", BTM_HEIGHT - 20, BTM_WIDTH, 0.4f, 0.4f, COL_TEXT_DIM);
    } else if (s_screen == SCREEN_NOW_PLAYING) {
        draw_header("Now Playing Controls");

        if (s_current_track_idx >= 0 && s_current_track_idx < s_num_tracks) {
            PlexTrack* track = &s_tracks[s_current_track_idx];
            draw_text_centered(track->title, 50, BTM_WIDTH, 0.6f, 0.6f, COL_TEXT);
            draw_text_centered(track->grandparent_title, 68, BTM_WIDTH, 0.45f, 0.45f, COL_ACCENT);

            // Seekable progress bar - tap anywhere on it to jump there.
            float progress = audio_player_get_progress();
            int pos_ms = audio_player_get_position_ms();
            float bar_x = 20, bar_y = 84, bar_w = BTM_WIDTH - 40, bar_h = 5;

            C2D_DrawRectSolid(bar_x, bar_y, 0.5f, bar_w, bar_h, COL_PROGRESS_BG);
            C2D_DrawRectSolid(bar_x, bar_y, 0.5f, bar_w * progress, bar_h, COL_ACCENT);
            C2D_DrawCircleSolid(bar_x + bar_w * progress, bar_y + bar_h / 2.0f, 0.5f, 6, COL_ACCENT);

            char tbuf1[32], tbuf2[32];
            format_time(pos_ms, tbuf1, sizeof(tbuf1));
            format_time(track->duration, tbuf2, sizeof(tbuf2));
            draw_text(tbuf1, bar_x, bar_y + 8, 0.35f, 0.35f, COL_TEXT_DIM);

            float t2_w, t2_h;
            C2D_Text txt2;
            C2D_TextParse(&txt2, s_text_buf, tbuf2);
            C2D_TextOptimize(&txt2);
            C2D_TextGetDimensions(&txt2, 0.35f, 0.35f, &t2_w, &t2_h);
            C2D_DrawText(&txt2, C2D_WithColor, bar_x + bar_w - t2_w, bar_y + 8, 0.5f, 0.35f, 0.35f, COL_TEXT_DIM);
        } else {
            draw_text_centered("No track playing", 65, BTM_WIDTH, 0.6f, 0.6f, COL_TEXT_DIM);
        }

        // Playback Buttons: [ Shuffle ] [ Prev ] [ Play/Pause ] [ Next ] [ Repeat ]
        // Icon-only (no text labels) using drawn shapes, not Unicode symbols -
        // see the icon functions' comment above draw_header().
        u32 dark_icon = C2D_Color32(0x12, 0x12, 0x18, 0xFF);

        C2D_DrawRectSolid(15, 108, 0.5f, 45, 45, s_shuffle_enabled ? COL_ACCENT : COL_SURFACE);
        draw_icon_shuffle(37, 130, 18, s_shuffle_enabled ? dark_icon : COL_TEXT_DIM);

        C2D_DrawRectSolid(65, 108, 0.5f, 55, 45, COL_SURFACE);
        draw_icon_skip_prev(92, 130, 20, COL_TEXT);

        PlayerState pbtn_state = audio_player_get_state();
        C2D_DrawRectSolid(125, 103, 0.5f, 70, 55, pbtn_state == PLAYER_LOADING ? COL_WARN : COL_ACCENT);
        if (pbtn_state == PLAYER_PLAYING) {
            draw_icon_pause(160, 130, 22, dark_icon);
        } else if (pbtn_state == PLAYER_LOADING) {
            draw_loading_spinner(160, 130, 14, NULL);
        } else {
            draw_icon_play(160, 130, 22, dark_icon);
        }

        C2D_DrawRectSolid(200, 108, 0.5f, 55, 45, COL_SURFACE);
        draw_icon_skip_next(227, 130, 20, COL_TEXT);

        C2D_DrawRectSolid(260, 108, 0.5f, 45, 45, s_repeat_mode != REPEAT_OFF ? COL_ACCENT : COL_SURFACE);
        draw_icon_repeat(282, 130, 20, s_repeat_mode != REPEAT_OFF ? dark_icon : COL_TEXT_DIM,
                          s_repeat_mode == REPEAT_ONE);

        // Lower Navigation Buttons: [ Up Next Queue ]  [ Synced Lyrics ]
        C2D_DrawRectSolid(20, 173, 0.5f, 135, 38, COL_HIGHLIGHT);
        draw_text_centered("Queue", 183, 175, 0.5f, 0.5f, COL_TEXT);

        C2D_DrawRectSolid(165, 173, 0.5f, 135, 38, COL_HIGHLIGHT);
        draw_text_centered("Lyrics", 183, 465, 0.5f, 0.5f, COL_ACCENT);

        draw_text_centered("Press SELECT or B to close", BTM_HEIGHT - 20, BTM_WIDTH, 0.4f, 0.4f, COL_TEXT_DIM);
    } else if (s_screen == SCREEN_QUEUE) {
        draw_header("Play Queue / Up Next");
        s_list_count = s_num_tracks;
        for (int i = 0; i < LIST_VISIBLE_ITEMS; i++) {
            int idx = s_list_offset + i;
            if (idx >= s_list_count) break;
            char dur_buf[32];
            format_time(s_tracks[idx].duration, dur_buf, sizeof(dur_buf));
            char qual_buf[32];
            format_quality_tag(&s_tracks[idx], qual_buf, sizeof(qual_buf));
            
            char sub_buf[96];
            if (s_tracks[idx].grandparent_title[0]) {
                if (qual_buf[0]) snprintf(sub_buf, sizeof(sub_buf), "%s  |  %s  |  %s", s_tracks[idx].grandparent_title, qual_buf, dur_buf);
                else snprintf(sub_buf, sizeof(sub_buf), "%s  |  %s", s_tracks[idx].grandparent_title, dur_buf);
            } else {
                if (qual_buf[0]) snprintf(sub_buf, sizeof(sub_buf), "%s  |  %s", qual_buf, dur_buf);
                else snprintf(sub_buf, sizeof(sub_buf), "%s", dur_buf);
            }
            bool playing = (idx == s_current_track_idx);
            draw_list_item(i, s_tracks[idx].title, sub_buf, idx == s_selected_idx, playing);
        }
    } else if (s_screen == SCREEN_LYRICS) {
        draw_header("Time-Synced Lyrics");
        int pos_ms = audio_player_get_position_ms();
        // Fills the header-to-bottom column height: base_y=44, half_window=3,
        // row_h=30 -> spans 44 to 224, just inside BTM_HEIGHT-15=225.
        draw_lyrics_block(pos_ms, 10, 44, BTM_WIDTH - 20, 30.0f, 3, 0.65f, 0.45f, COL_ACCENT, COL_TEXT_DIM);
    } else {
        if (s_screen == SCREEN_LIBRARIES) {
            draw_header("Music Libraries");
            if (s_list_count == 0) draw_loading_spinner(BTM_WIDTH / 2.0f, 130, 22, "Loading Libraries...");
        } else if (s_screen == SCREEN_ARTISTS) {
            draw_header(s_current_title[0] ? s_current_title : "Artists");
            if (s_list_count == 0) draw_loading_spinner(BTM_WIDTH / 2.0f, 130, 22, "Loading Artists...");
        } else if (s_screen == SCREEN_ALBUMS) {
            draw_header(s_current_title[0] ? s_current_title : "Albums");
            if (s_list_count == 0) draw_loading_spinner(BTM_WIDTH / 2.0f, 130, 22, "Loading Albums...");
        } else if (s_screen == SCREEN_TRACKS) {
            draw_header(s_current_title[0] ? s_current_title : "Tracks");
            if (s_list_count == 0) draw_loading_spinner(BTM_WIDTH / 2.0f, 130, 22, "Loading Tracks...");
        } else if (s_screen == SCREEN_PLAYLISTS) {
            draw_header("Playlists");
            if (s_list_count == 0) draw_loading_spinner(BTM_WIDTH / 2.0f, 130, 22, "Loading Playlists...");
        }
        
        for (int i = 0; i < LIST_VISIBLE_ITEMS; i++) {
            int idx = s_list_offset + i;
            if (idx >= s_list_count) break;
            
            bool selected = (idx == s_selected_idx);
            
            if (s_screen == SCREEN_LIBRARIES) {
                draw_list_item(i, s_libraries[idx].title, NULL, selected, false);
            } else if (s_screen == SCREEN_ARTISTS) {
                if (idx < s_num_artists) {
                    draw_list_item(i, s_artists[idx].title, NULL, selected, false);
                } else {
                    draw_list_item(i, "Loading...", NULL, selected, false);
                }
            } else if (s_screen == SCREEN_ALBUMS) {
                if (idx < s_num_albums) {
                    char year_buf[16];
                    snprintf(year_buf, sizeof(year_buf), "%d", s_albums[idx].year);
                    draw_list_item(i, s_albums[idx].title, s_albums[idx].year > 0 ? year_buf : NULL, selected, false);
                } else {
                    draw_list_item(i, "Loading...", NULL, selected, false);
                }
            } else if (s_screen == SCREEN_PLAYLISTS) {
                draw_list_item(i, s_playlists[idx].title, NULL, selected, false);
            } else if (s_screen == SCREEN_TRACKS) {
                if (idx < s_num_tracks) {
                    char dur_buf[32];
                    format_time(s_tracks[idx].duration, dur_buf, sizeof(dur_buf));
                    char qual_buf[32];
                    format_quality_tag(&s_tracks[idx], qual_buf, sizeof(qual_buf));
                    
                    char sub_buf[96];
                    if (s_tracks[idx].grandparent_title[0]) {
                        if (qual_buf[0]) {
                            snprintf(sub_buf, sizeof(sub_buf), "%s  |  %s  |  %s", s_tracks[idx].grandparent_title, qual_buf, dur_buf);
                        } else {
                            snprintf(sub_buf, sizeof(sub_buf), "%s  |  %s", s_tracks[idx].grandparent_title, dur_buf);
                        }
                    } else {
                        if (qual_buf[0]) {
                            snprintf(sub_buf, sizeof(sub_buf), "%s  |  %s", qual_buf, dur_buf);
                        } else {
                            snprintf(sub_buf, sizeof(sub_buf), "Track %d  |  %s", s_tracks[idx].index, dur_buf);
                        }
                    }
                    bool playing = (idx == s_current_track_idx);
                    draw_list_item(i, s_tracks[idx].title, sub_buf, selected, playing);
                } else {
                    draw_list_item(i, "Loading...", NULL, selected, false);
                }
            }
        }
    }
}
