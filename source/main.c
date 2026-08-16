#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <curl/curl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <time.h>

#include "config.h"
#include "plex_api.h"
#include "audio_player.h"
#include "offline.h"
#include "library_cache.h"
#include "ui.h"
#include "logger.h"

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

static u32* soc_buffer = NULL;
static AppConfig app_config;

int main(int argc, char* argv[]) {
    // Enable New 3DS 804MHz CPU Clock Speedup & L2 Cache
    osSetSpeedupEnable(true);

    // Seed rand() (used for shuffle) - without this it produces the exact
    // same sequence every launch, since nothing else in the app called
    // srand(). svcGetSystemTick() varies even if the RTC hasn't been set.
    srand((unsigned int)(time(NULL) ^ svcGetSystemTick()));

    // Initialize services
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    ndspInit();
    ptmuInit(); // needed for the battery HUD's charging-state read (PTMU_GetBatteryChargeState) -
                // unlike hid/gfx/apt, ptm:u isn't started by the default runtime init
    mcuHwcInit(); // needed for the battery HUD's percentage (MCUHWC_GetBatteryLevel) - see
                  // ui_render_top()'s comment on why this, not PTMU_GetBatteryLevel, is used for it
    
    // Initialize network
    soc_buffer = (u32*)memalign(SOC_ALIGN, SOC_BUFFERSIZE);
    if (soc_buffer) {
        socInit(soc_buffer, SOC_BUFFERSIZE);
    }
    
    // Initialize curl
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    // Create render targets
    C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    
    // Load config
    config_set_defaults(&app_config);
    config_load(&app_config);
    config_ensure_client_id(&app_config); // per-console id - see config_ensure_client_id()

    // Must happen before any plex_api_* call, including the login-flow ones
    // (plex_api_create_pin()/plex_api_login_direct()) triggered later from
    // the UI - see plex_api_set_client_id().
    plex_api_set_client_id(app_config.client_id);

    // Initialize subsystems
    logger_init();
    LOG_INFO("DualPlex starting up...");
    audio_player_init();
    offline_init();
    library_cache_init();
    ui_init();
    ui_set_config(&app_config);
    
    // If config has valid server info, try to connect
    bool connected = false;
    if (app_config.server_url[0] && app_config.auth_token[0]) {
        // This can block for several seconds (worst case, the reconnect-
        // via-account fallback below retrying every address it knows for
        // the server in turn) - draw one frame first so the console shows
        // something other than a blank/stale screen while that's happening.
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        ui_render_connecting(top, bottom);
        C3D_FrameEnd(0);

        plex_api_init(app_config.server_url, app_config.auth_token);
        if (plex_api_test_connection()) {
            connected = true;
        } else {
            // The saved address didn't work - this happens whenever the
            // console launches on a different network than last time (a
            // mobile hotspot, a different WiFi) where that particular
            // address (typically the server's LAN IP) just isn't reachable,
            // even though the server itself is still up and has other
            // addresses that would work fine from here. Re-fetch the
            // account's server list with the saved token and retry every
            // address it advertises for the same server, rather than
            // dropping straight to the login screen as if logged out.
            LOG_WARN("Saved server address unreachable - attempting reconnect via account");
            if (plex_api_reconnect_via_account(app_config.auth_token, app_config.server_name)) {
                connected = true;
                // Persist whatever address actually worked so the next
                // launch tries it first instead of repeating this fallback.
                strncpy(app_config.server_url, plex_api_get_server_url(), sizeof(app_config.server_url) - 1);
                strncpy(app_config.auth_token, plex_api_get_token(), sizeof(app_config.auth_token) - 1);
                config_save(&app_config);
            }
        }

        if (connected) ui_set_screen(SCREEN_HUB);
    }

    if (!connected) {
        ui_set_screen(SCREEN_AUTH_CHOICE);
    }
    
    // Main loop
    bool sleep_allowed = true; // libctru's own default, tracked so we only call
                                // aptSetSleepAllowed() on actual transitions
    while (aptMainLoop()) {
        // Input
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();
        touchPosition touch;
        hidTouchRead(&touch);

        // Exit on START+SELECT - checked both ways round (whichever of the
        // two is the one that lands as the actual "just pressed" edge this
        // frame, with the other already held) since a real two-finger press
        // is rarely perfectly simultaneous. Checking only one ordering (e.g.
        // START-down-while-SELECT-held) misses the combo whenever the user
        // happens to press START fractionally before SELECT instead - it
        // never satisfies either half that way (by the frame SELECT goes
        // down, START is no longer a fresh kDown edge itself).
        if (((kDown & KEY_START) && (kHeld & KEY_SELECT)) ||
            ((kDown & KEY_SELECT) && (kHeld & KEY_START))) {
            break;
        }

        // Keep playback going with the lid closed: closing the lid always cuts
        // the screen backlights (that's hardware, not something we control),
        // but by default it also tells the OS to fully suspend the console -
        // which would stop NDSP audio and the network stream mid-track. Disable
        // that suspend only while a track is actively playing, so closing the
        // lid works like a phone with the screen off instead of pausing the
        // console; sleep behaves normally the rest of the time (idle on a menu,
        // paused, stopped) to not cost battery for no reason.
        bool should_allow_sleep = (audio_player_get_state() != PLAYER_PLAYING);
        if (should_allow_sleep != sleep_allowed) {
            aptSetSleepAllowed(should_allow_sleep);
            sleep_allowed = should_allow_sleep;
        }

        // Update
        audio_player_update();
        offline_set_network_streaming_hint(audio_player_is_streaming_over_network()); // for the download thread's bandwidth throttle - see offline.h
        // No offline_*_update() call needed here - downloads run entirely on
        // their own background thread now (see offline_init()), with
        // nothing left that has to be pumped from the main loop.
        ui_update(kDown, kHeld, touch);
        
        // Render
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        
        C2D_TargetClear(top, C2D_Color32(0x12, 0x12, 0x24, 0xFF));
        C2D_SceneBegin(top);
        ui_render_top(top);
        
        C2D_TargetClear(bottom, C2D_Color32(0x1A, 0x1A, 0x2E, 0xFF));
        C2D_SceneBegin(bottom);
        ui_render_bottom(bottom);
        
        C3D_FrameEnd(0);
    }

    // Restore normal sleep behavior in case we're exiting mid-playback with it disabled
    if (!sleep_allowed) aptSetSleepAllowed(true);

    // Save config before exit
    config_save(&app_config);
    
    // Cleanup (reverse order)
    ui_cleanup();
    audio_player_cleanup();
    offline_cleanup();
    plex_api_cleanup();
    curl_global_cleanup();
    
    if (soc_buffer) {
        socExit();
        free(soc_buffer);
    }
    
    ndspExit();
    ptmuExit();
    mcuHwcExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    
    return 0;
}
