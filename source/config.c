#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <unistd.h>
#else
#include <direct.h>
#endif

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
    config->volume = 80;
}

bool config_load(AppConfig* config) {
    if (!config) return false;
    
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return false;
    
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
                } else if (strcmp(key, "volume") == 0) {
                    config->volume = atoi(val);
                }
            }
        }
    }
    fclose(f);
    return true;
}

bool config_save(const AppConfig* config) {
    if (!config) return false;
    
    create_dir_recursive(CONFIG_DIR);
    
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return false;
    
    fprintf(f, "server_url=%s\n", config->server_url);
    fprintf(f, "auth_token=%s\n", config->auth_token);
    fprintf(f, "volume=%d\n", config->volume);
    
    fclose(f);
    return true;
}
