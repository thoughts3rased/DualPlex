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
} AppConfig;

// Load config from SD card. Returns true on success.
bool config_load(AppConfig* config);

// Save config to SD card. Returns true on success.
bool config_save(const AppConfig* config);

// Set default values.
void config_set_defaults(AppConfig* config);

#endif // CONFIG_H
