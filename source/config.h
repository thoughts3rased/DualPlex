#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

#define PLEX_MAX_STR 256
#define PLEX_MAX_URL 2048
#define CONFIG_DIR "/3ds/dualplex"
#define CONFIG_PATH "/3ds/dualplex/config.txt"

typedef struct {
    char server_url[PLEX_MAX_URL];  // e.g. "http://192.168.1.100:32400"
    char auth_token[128];            // X-Plex-Token
    // Name of the Plex server resource server_url/auth_token were picked
    // from (SCREEN_SERVER_SELECT), e.g. "Living Room Server" - blank if
    // set up via a manually-typed URL instead. Lets a later launch that
    // can't reach server_url directly (a different network - a mobile
    // hotspot, say - where that particular address isn't reachable) match
    // this same server back up in a re-fetched resource list and retry
    // its other advertised addresses, rather than just giving up. See
    // plex_api_reconnect_via_account().
    char server_name[PLEX_MAX_STR];
    // When true, server_url is a user-set override (Settings > Server
    // Address, on the Hub) that always wins: startup only ever tries this
    // exact address and never falls back to plex.tv account discovery when
    // it fails (see main.c's use of plex_api_reconnect_via_account()). Off
    // by default - a server_url saved via the normal sign-in/server-select
    // flow (SCREEN_SERVER_SELECT) is just a "last known good" address, not
    // an override, so a failed connection there still retries every other
    // address plex.tv knows about for the account. Added because Plex's own
    // connection ordering (see plex_api.c's build_connection_candidates())
    // can't always guess right for every network/router combination - this
    // is the escape hatch for when it doesn't.
    bool server_url_locked;
    // Whether the top-screen clock displays 24-hour time instead of 12-hour
    // with AM/PM. Not read from the 3DS's own "Display 24-Hour Time" system
    // setting - that isn't exposed through any documented API - so this is
    // DualPlex's own remembered preference instead (see Settings screen).
    bool clock_24h;
    // Randomly-generated identifier unique to this physical console, sent
    // as Plex's X-Plex-Client-Identifier header on every request (see
    // plex_api_set_client_id()). Generated once by config_ensure_client_id()
    // below and persisted here so it stays stable across relaunches - PMS
    // keys its Now Playing dashboard "players" by this value, so without a
    // stable per-console id, two 3DS's signed into the same account would
    // look like a single player to it.
    char client_id[48];
} AppConfig;

// Load config from SD card. Returns true on success.
bool config_load(AppConfig* config);

// Save config to SD card. Returns true on success.
bool config_save(const AppConfig* config);

// Set default values.
void config_set_defaults(AppConfig* config);

// Fills in config->client_id with a freshly generated value and persists it
// via config_save() if it's not already set (a fresh install, or a
// config.txt saved before this field existed). A no-op otherwise. Call
// after srand() has been seeded (see main.c) and before any plex_api_* call.
void config_ensure_client_id(AppConfig* config);

#endif // CONFIG_H
