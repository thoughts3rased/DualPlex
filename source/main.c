#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <curl/curl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#include "config.h"
#include "plex_api.h"
#include "audio_player.h"
#include "ui.h"
#include "logger.h"

#define SOC_ALIGN 0x1000
#define SOC_BUFFERSIZE 0x100000

static u32* soc_buffer = NULL;
static AppConfig app_config;

int main(int argc, char* argv[]) {
    // Enable New 3DS 804MHz CPU Clock Speedup & L2 Cache
    osSetSpeedupEnable(true);
    
    // Initialize services
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    ndspInit();
    ptmuInit(); // needed for the battery HUD (PTMU_GetBatteryLevel/ChargeState) -
                // unlike hid/gfx/apt, ptm:u isn't started by the default runtime init
    
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
    
    // Initialize subsystems
    logger_init();
    LOG_INFO("DualPlex starting up...");
    audio_player_init();
    ui_init();
    ui_set_config(&app_config);
    
    // If config has valid server info, try to connect
    bool connected = false;
    if (app_config.server_url[0] && app_config.auth_token[0]) {
        plex_api_init(app_config.server_url, app_config.auth_token);
        if (plex_api_test_connection()) {
            ui_set_screen(SCREEN_HUB);
            connected = true;
        }
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

        // Exit on START+SELECT
        if ((kDown & KEY_START) && (kHeld & KEY_SELECT)) break;

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
    plex_api_cleanup();
    curl_global_cleanup();
    
    if (soc_buffer) {
        socExit();
        free(soc_buffer);
    }
    
    ndspExit();
    ptmuExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    
    return 0;
}
