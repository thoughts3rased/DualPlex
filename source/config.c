#include "config.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <direct.h>
#endif

// Pre-rename config location (app was called "3DS Plex Client"). Kept only so
// config_load() can fall back to it and migrate existing installs forward.
#define OLD_CONFIG_PATH "/3ds/3ds-plex-client/config.txt"

static void create_dir_recursive(const char* dir) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
#ifndef _WIN32
            mkdir(tmp, S_IRWXU);
#else
            _mkdir(tmp);
#endif
            *p = '/';
        }
    }
#ifndef _WIN32
    mkdir(tmp, S_IRWXU);
#else
    _mkdir(tmp);
#endif
}

void config_set_defaults(AppConfig* config) {
    if (!config) return;
    memset(config, 0, sizeof(AppConfig));
}

static void parse_config_file(FILE* f, AppConfig* config) {
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[128] = {0};
        char val[512] = {0};

        // Remove trailing newline if present
        size_t len = strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (len > 1 && line[len-2] == '\r') line[len-2] = '\0';

        char* eq = strchr(line, '=');
        if (eq) {
            size_t key_len = eq - line;
            if (key_len < sizeof(key)) {
                strncpy(key, line, key_len);
                key[key_len] = '\0';
                strncpy(val, eq + 1, sizeof(val) - 1);

                if (strcmp(key, "server_url") == 0) {
                    strncpy(config->server_url, val, sizeof(config->server_url) - 1);
                } else if (strcmp(key, "auth_token") == 0) {
                    strncpy(config->auth_token, val, sizeof(config->auth_token) - 1);
                }
                // "volume" is intentionally no longer read: the app has no volume
                // setting of its own anymore (see config_save() below) - kept
                // silently ignored here so old config.txt files with a leftover
                // volume= line don't need to be hand-edited.
            }
        }
    }
}

bool config_load(AppConfig* config) {
    if (!config) return false;

    FILE *f = fopen(CONFIG_PATH, "r");
    if (f) {
        parse_config_file(f, config);
        fclose(f);
        return true;
    }

    // Fall back to the pre-rename config location so existing installs
    // (from when the app was "3DS Plex Client") keep working after the update.
    f = fopen(OLD_CONFIG_PATH, "r");
    if (!f) return false;

    LOG_INFO("No config at %s - found legacy config at %s, migrating", CONFIG_PATH, OLD_CONFIG_PATH);
    parse_config_file(f, config);
    fclose(f);

    // Best-effort migration to the new location; if it fails we'll just read
    // from the old path again next launch.
    config_save(config);

    return true;
}

bool config_save(const AppConfig* config) {
    if (!config) return false;
    
    create_dir_recursive(CONFIG_DIR);
    
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return false;
    
    fprintf(f, "server_url=%s\n", config->server_url);
    fprintf(f, "auth_token=%s\n", config->auth_token);
    
    fclose(f);
    return true;
}
