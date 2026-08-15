#include "plex_api.h"
#include "logger.h"
#include "audio_player.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <curl/curl.h>
#include "lib/cJSON.h"

static char s_server_url[PLEX_MAX_URL] = {0};
static char s_auth_token[128] = {0};
static bool s_initialized = false;
static PlexQualityTier s_quality_tier = QUALITY_MP3_320;  // Default: highest MP3

static int quality_tier_to_bitrate(PlexQualityTier tier) {
    switch (tier) {
        case QUALITY_FLAC_DIRECT: return 0;   // Direct stream, no transcode
        case QUALITY_MP3_320:     return 320;
        case QUALITY_MP3_192:     return 192;
        case QUALITY_MP3_128:     return 128;
        case QUALITY_MP3_64:      return 64;
        default:                  return 128;
    }
}

PlexQualityTier plex_api_get_quality_tier(void) { return s_quality_tier; }
void plex_api_set_quality_tier(PlexQualityTier tier) {
    if (tier >= 0 && tier < QUALITY_TIER_COUNT) {
        s_quality_tier = tier;
        LOG_INFO("Quality tier set to: %s", plex_api_get_quality_label(tier));
    }
}

bool plex_api_quality_step_down(void) {
    if (s_quality_tier >= QUALITY_MP3_64) return false;
    s_quality_tier++;
    // Skip FLAC_DIRECT if stepping down from it
    if (s_quality_tier == QUALITY_FLAC_DIRECT) s_quality_tier = QUALITY_MP3_320;
    LOG_WARN("Quality stepped down to: %s", plex_api_get_quality_label(s_quality_tier));
    return true;
}

const char* plex_api_get_quality_label(PlexQualityTier tier) {
    switch (tier) {
        case QUALITY_FLAC_DIRECT: return "FLAC Direct";
        case QUALITY_MP3_320:     return "MP3 320k";
        case QUALITY_MP3_192:     return "MP3 192k";
        case QUALITY_MP3_128:     return "MP3 128k";
        case QUALITY_MP3_64:      return "MP3 64k";
        default:                  return "Unknown";
    }
}

PlexQualityTier plex_api_suggest_quality(int download_speed_bps, bool is_n3ds) {
    // Convert bytes/sec to kbits/sec with safety margin (need ~1.5x headroom)
    int available_kbps = (download_speed_bps * 8) / 1000;
    int effective_kbps = available_kbps * 2 / 3;  // Use 2/3 of measured bandwidth
    
    LOG_INFO("Network speed: %d KB/s (%d kbps available, %d kbps effective)",
             download_speed_bps / 1024, available_kbps, effective_kbps);
    
    if (is_n3ds && effective_kbps >= 1500) return QUALITY_FLAC_DIRECT;
    if (effective_kbps >= 480)             return QUALITY_MP3_320;
    if (effective_kbps >= 288)             return QUALITY_MP3_192;
    if (effective_kbps >= 192)             return QUALITY_MP3_128;
    return QUALITY_MP3_64;
}


typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} HttpBuffer;

static size_t http_write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    HttpBuffer* mem = (HttpBuffer*)userp;
    
    char* ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        return 0; // out of memory
    }
    
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    
    return realsize;
}

// Runs a single already-configured curl easy handle to completion, like
// curl_easy_perform() - but via curl_multi polling instead of one opaque
// blocking call, pumping audio_player_update() in between so a slow request
// (browsing a big library, a laggy server, a login) doesn't starve the NDSP
// wave buffers and audibly interrupt whatever's currently playing. Callers
// still get a synchronous call that only returns once the request is done;
// only what happens *during* the wait changes.
static CURLcode perform_blocking_request(CURL* curl) {
    CURLM* multi = curl_multi_init();
    if (!multi) {
        return curl_easy_perform(curl); // degrade to plain blocking rather than fail outright
    }
    curl_multi_add_handle(multi, curl);

    int still_running = 1;
    CURLMcode mc;
    do {
        mc = curl_multi_perform(multi, &still_running);
        if (mc != CURLM_OK) break;
        if (still_running) {
            audio_player_update();
            int numfds = 0;
            curl_multi_wait(multi, NULL, 0, 50, &numfds); // wait up to 50ms for socket activity
        }
    } while (still_running);

    CURLcode result = (mc == CURLM_OK) ? CURLE_GOT_NOTHING : CURLE_RECV_ERROR; // overwritten below on normal completion
    CURLMsg* msg;
    int msgs_left = 0;
    while ((msg = curl_multi_info_read(multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) result = msg->data.result;
    }

    curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    return result;
}

static bool plex_http_get(const char* endpoint, char** response_out) {
    if (!s_initialized) return false;
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    char url[PLEX_MAX_URL];
    snprintf(url, sizeof(url), "%s%s", s_server_url, endpoint);
    
    HttpBuffer chunk;
    chunk.data = malloc(1);
    chunk.size = 0;
    chunk.capacity = 1;
    if (chunk.data) chunk.data[0] = 0;
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    
    char token_header[256];
    snprintf(token_header, sizeof(token_header), "X-Plex-Token: %s", s_auth_token);
    headers = curl_slist_append(headers, token_header);
    
    headers = curl_slist_append(headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    headers = curl_slist_append(headers, "X-Plex-Product: " PLEX_PRODUCT);
    headers = curl_slist_append(headers, "X-Plex-Version: " PLEX_VERSION);
    headers = curl_slist_append(headers, "X-Plex-Device: " PLEX_DEVICE);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res = perform_blocking_request(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res == CURLE_OK && http_code == 200 && chunk.data) {
        *response_out = chunk.data;
        return true;
    }
    
    if (chunk.data) free(chunk.data);
    *response_out = NULL;
    return false;
}

static bool plex_http_get_full_url(const char* url, const char* token, char** response_out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    HttpBuffer chunk;
    chunk.data = malloc(1);
    chunk.size = 0;
    chunk.capacity = 1;
    if (chunk.data) chunk.data[0] = 0;
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (token && token[0]) {
        char token_hdr[256];
        snprintf(token_hdr, sizeof(token_hdr), "X-Plex-Token: %s", token);
        headers = curl_slist_append(headers, token_hdr);
    }
    headers = curl_slist_append(headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    headers = curl_slist_append(headers, "X-Plex-Product: " PLEX_PRODUCT);
    headers = curl_slist_append(headers, "X-Plex-Version: " PLEX_VERSION);
    headers = curl_slist_append(headers, "X-Plex-Device: " PLEX_DEVICE);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res = perform_blocking_request(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    if (http_code > 0 && (http_code < 200 || http_code >= 300)) {
        LOG_ERROR("[HTTP NON-2XX RESPONSE %ld] API request failed for URL: %s", http_code, url);
    }
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res == CURLE_OK && http_code >= 200 && http_code < 300 && chunk.data) {
        *response_out = chunk.data;
        return true;
    }
    
    if (chunk.data) free(chunk.data);
    *response_out = NULL;
    return false;
}

static bool plex_http_post_full_url(const char* url, const char* post_fields, const char* token, char** response_out, long* status_out) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    HttpBuffer chunk;
    chunk.data = malloc(1);
    chunk.size = 0;
    chunk.capacity = 1;
    if (chunk.data) chunk.data[0] = 0;
    
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (token && token[0]) {
        char token_hdr[256];
        snprintf(token_hdr, sizeof(token_hdr), "X-Plex-Token: %s", token);
        headers = curl_slist_append(headers, token_hdr);
    }
    headers = curl_slist_append(headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    headers = curl_slist_append(headers, "X-Plex-Product: " PLEX_PRODUCT);
    headers = curl_slist_append(headers, "X-Plex-Version: " PLEX_VERSION);
    headers = curl_slist_append(headers, "X-Plex-Device: " PLEX_DEVICE);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (post_fields) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
    } else {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    CURLcode res = perform_blocking_request(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (status_out) *status_out = http_code;
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res == CURLE_OK && chunk.data) {
        *response_out = chunk.data;
        return true;
    }
    
    if (chunk.data) free(chunk.data);
    *response_out = NULL;
    return false;
}

static void sanitize_server_url(const char* in_url, char* out_url, size_t max_len) {
    if (!in_url || !in_url[0]) {
        out_url[0] = '\0';
        return;
    }
    
    char temp[PLEX_MAX_URL];
    strncpy(temp, in_url, sizeof(temp) - 1);
    temp[sizeof(temp) - 1] = '\0';
    
    // Check if domain is a .plex.direct address with dashed IP (e.g. 192-168-0-200.xyz.plex.direct:32400)
    char* direct = strstr(temp, ".plex.direct");
    if (direct) {
        char* ip_start = strstr(temp, "http://");
        if (ip_start) ip_start += 7;
        else {
            ip_start = strstr(temp, "https://");
            if (ip_start) ip_start += 8;
            else ip_start = temp;
        }
        
        int a, b, c, d, port = 32400;
        if (sscanf(ip_start, "%d-%d-%d-%d", &a, &b, &c, &d) == 4) {
            char* port_str = strchr(direct, ':');
            if (port_str) port = atoi(port_str + 1);
            snprintf(out_url, max_len, "http://%d.%d.%d.%d:%d", a, b, c, d, port > 0 ? port : 32400);
            return;
        }
    }
    
    // Convert https:// to http:// for local IP connections
    if (strncmp(temp, "https://", 8) == 0) {
        snprintf(out_url, max_len, "http://%s", temp + 8);
        return;
    }
    
    strncpy(out_url, temp, max_len - 1);
    out_url[max_len - 1] = '\0';
}

bool plex_api_init(const char* server_url, const char* auth_token) {
    sanitize_server_url(server_url, s_server_url, sizeof(s_server_url));
    strncpy(s_auth_token, auth_token, sizeof(s_auth_token) - 1);
    
    // Remove trailing slash if present
    size_t len = strlen(s_server_url);
    if (len > 0 && s_server_url[len - 1] == '/') {
        s_server_url[len - 1] = '\0';
    }
    
    s_initialized = true;
    return true;
}

void plex_api_cleanup(void) {
    s_initialized = false;
    s_server_url[0] = '\0';
    s_auth_token[0] = '\0';
}

const char* plex_api_get_token(void) {
    return s_auth_token;
}

const char* plex_api_get_server_url(void) {
    return s_server_url;
}

bool plex_api_is_https(void) {
    return strncmp(s_server_url, "https://", 8) == 0;
}

bool plex_api_is_local_connection(void) {
    // Parsed straight from the stored, sanitized server URL rather than
    // relying on PlexServerResource.is_local - that flag only exists when
    // the server was picked from plex.tv's resource list (plex_api_get_
    // servers() above), not when the user typed a URL manually in Setup.
    // Reading it back off the URL itself covers both paths and always
    // matches the address actually being connected to.
    const char* host = strstr(s_server_url, "://");
    host = host ? host + 3 : s_server_url;

    int a, b;
    if (sscanf(host, "%d.%d.", &a, &b) != 2) {
        // Not a bare dotted-quad IP (a DNS hostname, e.g. a custom domain
        // or a plain plex.direct name that sanitize_server_url() didn't
        // resolve to an IP) - treat as remote, since private ranges are
        // only ever addressed by literal IP.
        return false;
    }
    if (a == 10) return true;                        // 10.0.0.0/8
    if (a == 172 && b >= 16 && b <= 31) return true;  // 172.16.0.0/12
    if (a == 192 && b == 168) return true;            // 192.168.0.0/16
    if (a == 127) return true;                        // loopback
    if (a == 169 && b == 254) return true;            // link-local
    return false;
}

bool plex_api_test_connection(void) {
    char* response = NULL;
    if (plex_http_get("/", &response)) {
        free(response);
        return true;
    }
    return false;
}

bool plex_api_create_pin(PlexPin* out_pin) {
    if (!out_pin) return false;
    memset(out_pin, 0, sizeof(PlexPin));
    
    char* response = NULL;
    if (plex_http_post_full_url("https://plex.tv/api/v2/pins", "", NULL, &response, NULL)) {
        cJSON* json = cJSON_Parse(response);
        if (json) {
            cJSON* id = cJSON_GetObjectItem(json, "id");
            cJSON* code = cJSON_GetObjectItem(json, "code");
            if (id && cJSON_IsNumber(id)) out_pin->id = id->valueint;
            if (code && cJSON_IsString(code)) {
                strncpy(out_pin->code, code->valuestring, sizeof(out_pin->code) - 1);
            }
            cJSON_Delete(json);
        }
        free(response);
    }
    return out_pin->code[0] != '\0';
}

bool plex_api_check_pin(PlexPin* pin) {
    if (!pin || pin->id == 0) return false;
    
    char url[128];
    snprintf(url, sizeof(url), "https://plex.tv/api/v2/pins/%d", pin->id);
    
    char* response = NULL;
    if (plex_http_get_full_url(url, NULL, &response)) {
        cJSON* json = cJSON_Parse(response);
        if (json) {
            cJSON* token = cJSON_GetObjectItem(json, "authToken");
            if (token && cJSON_IsString(token) && token->valuestring[0] != '\0') {
                strncpy(pin->auth_token, token->valuestring, sizeof(pin->auth_token) - 1);
                cJSON_Delete(json);
                free(response);
                return true;
            }
            cJSON* expired = cJSON_GetObjectItem(json, "expired");
            if (expired && cJSON_IsBool(expired)) pin->expired = cJSON_IsTrue(expired);
            cJSON_Delete(json);
        }
        free(response);
    }
    return false;
}

// Curl URL encoding helper
static void url_encode_cat(char* dest, size_t dest_max, const char* src) {
    CURL* curl = curl_easy_init();
    if (curl) {
        char* encoded = curl_easy_escape(curl, src, 0);
        if (encoded) {
            strncat(dest, encoded, dest_max - strlen(dest) - 1);
            curl_free(encoded);
        }
        curl_easy_cleanup(curl);
    }
}

int plex_api_login_direct(const char* login, const char* password, const char* code_2fa, char* out_token, size_t max_token) {
    if (!login || !password || !out_token) return 0;
    out_token[0] = '\0';
    
    char post_fields[1024] = "rememberMe=true&user%5Blogin%5D=";
    url_encode_cat(post_fields, sizeof(post_fields), login);
    strcat(post_fields, "&user%5Bpassword%5D=");
    url_encode_cat(post_fields, sizeof(post_fields), password);
    
    if (code_2fa && code_2fa[0] != '\0') {
        strcat(post_fields, "&user%5Bverification_code%5D=");
        url_encode_cat(post_fields, sizeof(post_fields), code_2fa);
    }
    
    char* response = NULL;
    long status = 0;
    bool req_ok = plex_http_post_full_url("https://plex.tv/api/v2/users/signin", post_fields, NULL, &response, &status);
    
    int result = 0;
    if (req_ok && response) {
        cJSON* json = cJSON_Parse(response);
        if (json) {
            // Check for authToken at top level or under "user"
            cJSON* token = cJSON_GetObjectItem(json, "authToken");
            if (!token) {
                cJSON* user = cJSON_GetObjectItem(json, "user");
                if (user) token = cJSON_GetObjectItem(user, "authToken");
            }
            if (token && cJSON_IsString(token) && token->valuestring[0] != '\0') {
                strncpy(out_token, token->valuestring, max_token - 1);
                result = 1; // Success
            } else {
                // Check if errors array indicates 2FA required
                cJSON* errors = cJSON_GetObjectItem(json, "errors");
                if (errors || status == 401 || status == 422) {
                    result = 2; // 2FA Code required or invalid credentials
                }
            }
            cJSON_Delete(json);
        } else if (status == 401 || status == 422) {
            result = 2;
        }
        free(response);
    } else if (status == 401 || status == 422) {
        result = 2;
    }
    
    return result;
}

int plex_api_get_servers(const char* account_token, PlexServerResource* out_servers, int max) {
    if (!account_token || !out_servers) return 0;
    
    char* response = NULL;
    if (!plex_http_get_full_url("https://plex.tv/api/v2/resources?includeHttps=1", account_token, &response)) {
        return 0;
    }
    
    int count = 0;
    cJSON* json = cJSON_Parse(response);
    if (json && cJSON_IsArray(json)) {
        cJSON* res_item = NULL;
        cJSON_ArrayForEach(res_item, json) {
            if (count >= max) break;
            
            cJSON* provides = cJSON_GetObjectItem(res_item, "provides");
            if (provides && cJSON_IsString(provides) && strstr(provides->valuestring, "server")) {
                cJSON* name = cJSON_GetObjectItem(res_item, "name");
                cJSON* token = cJSON_GetObjectItem(res_item, "accessToken");
                
                if (name && cJSON_IsString(name)) {
                    strncpy(out_servers[count].name, name->valuestring, sizeof(out_servers[count].name) - 1);
                }
                if (token && cJSON_IsString(token)) {
                    strncpy(out_servers[count].access_token, token->valuestring, sizeof(out_servers[count].access_token) - 1);
                } else {
                    strncpy(out_servers[count].access_token, account_token, sizeof(out_servers[count].access_token) - 1);
                }
                
                // Parse connections: prefer local direct http://IP:port
                cJSON* connections = cJSON_GetObjectItem(res_item, "connections");
                if (connections && cJSON_IsArray(connections)) {
                    cJSON* conn = NULL;
                    char best_uri[PLEX_MAX_URL] = "";
                    bool best_is_local = false;
                    
                    cJSON_ArrayForEach(conn, connections) {
                        cJSON* uri = cJSON_GetObjectItem(conn, "uri");
                        cJSON* local = cJSON_GetObjectItem(conn, "local");
                        cJSON* address = cJSON_GetObjectItem(conn, "address");
                        cJSON* port = cJSON_GetObjectItem(conn, "port");
                        
                        bool is_loc = (local && cJSON_IsTrue(local));
                        char direct_http[PLEX_MAX_URL] = "";
                        if (is_loc && address && cJSON_IsString(address) && port && cJSON_IsNumber(port)) {
                            snprintf(direct_http, sizeof(direct_http), "http://%s:%d", address->valuestring, port->valueint);
                        }
                        
                        const char* target = direct_http[0] ? direct_http : (uri && cJSON_IsString(uri) ? uri->valuestring : "");
                        
                        if (target[0] != '\0') {
                            if (is_loc && direct_http[0]) {
                                strncpy(best_uri, target, sizeof(best_uri) - 1);
                                best_is_local = true;
                                break; // Best connection found!
                            } else if (is_loc && !best_is_local) {
                                strncpy(best_uri, target, sizeof(best_uri) - 1);
                                best_is_local = true;
                            } else if (best_uri[0] == '\0') {
                                strncpy(best_uri, target, sizeof(best_uri) - 1);
                            }
                        }
                    }
                    if (best_uri[0] != '\0') {
                        sanitize_server_url(best_uri, out_servers[count].uri, sizeof(out_servers[count].uri));
                        out_servers[count].is_local = best_is_local;
                        count++;
                    }
                }
            }
        }
        cJSON_Delete(json);
    }
    
    if (response) free(response);
    return count;
}

int plex_api_get_music_libraries(PlexLibrary* out, int max) {
    char* response = NULL;
    if (!plex_http_get("/library/sections", &response)) {
        return 0;
    }
    
    int count = 0;
    cJSON* json = cJSON_Parse(response);
    if (json) {
        cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
        if (container) {
            cJSON* directory = cJSON_GetObjectItem(container, "Directory");
            if (directory && cJSON_IsArray(directory)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, directory) {
                    if (count >= max) break;
                    
                    cJSON* type_json = cJSON_GetObjectItem(item, "type");
                    if (type_json && cJSON_IsString(type_json) && strcmp(type_json->valuestring, "artist") == 0) {
                        cJSON* key = cJSON_GetObjectItem(item, "key");
                        cJSON* title = cJSON_GetObjectItem(item, "title");
                        
                        if (key && cJSON_IsString(key)) {
                            snprintf(out[count].key, sizeof(out[count].key), "/library/sections/%s/all", key->valuestring);
                        }
                        if (title && cJSON_IsString(title)) {
                            strncpy(out[count].title, title->valuestring, sizeof(out[count].title) - 1);
                        }
                        strncpy(out[count].type, "artist", sizeof(out[count].type) - 1);
                        
                        count++;
                    }
                }
            }
        }
        cJSON_Delete(json);
    }
    
    free(response);
    return count;
}

int plex_api_get_artists_page(const char* library_key, PlexArtist* out, int start, int count, int* out_total) {
    if (out_total) *out_total = 0;
    if (!library_key || !out) return 0;
    
    char endpoint[512];
    if (strstr(library_key, "?")) {
        snprintf(endpoint, sizeof(endpoint), "%s&X-Plex-Container-Start=%d&X-Plex-Container-Size=%d", library_key, start, count);
    } else {
        snprintf(endpoint, sizeof(endpoint), "%s?X-Plex-Container-Start=%d&X-Plex-Container-Size=%d", library_key, start, count);
    }
    
    char* response = NULL;
    if (!plex_http_get(endpoint, &response)) {
        return 0;
    }
    
    int items_parsed = 0;
    cJSON* json = cJSON_Parse(response);
    if (json) {
        cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
        if (container) {
            cJSON* totalSize = cJSON_GetObjectItem(container, "totalSize");
            if (totalSize && cJSON_IsNumber(totalSize) && out_total) {
                *out_total = totalSize->valueint;
            }
            
            cJSON* metadata = cJSON_GetObjectItem(container, "Metadata");
            if (metadata && cJSON_IsArray(metadata)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, metadata) {
                    int idx = start + items_parsed;
                    if (idx >= PLEX_MAX_ITEMS) break;
                    memset(&out[idx], 0, sizeof(PlexArtist));
                    
                    cJSON* ratingKey = cJSON_GetObjectItem(item, "ratingKey");
                    cJSON* key = cJSON_GetObjectItem(item, "key");
                    cJSON* title = cJSON_GetObjectItem(item, "title");
                    cJSON* summary = cJSON_GetObjectItem(item, "summary");
                    cJSON* thumb = cJSON_GetObjectItem(item, "thumb");
                    
                    if (ratingKey) {
                        if (cJSON_IsString(ratingKey) && ratingKey->valuestring) strncpy(out[idx].rating_key, ratingKey->valuestring, sizeof(out[idx].rating_key) - 1);
                        else if (cJSON_IsNumber(ratingKey)) snprintf(out[idx].rating_key, sizeof(out[idx].rating_key), "%d", ratingKey->valueint);
                    }
                    if (key && cJSON_IsString(key)) {
                        if (strstr(key->valuestring, "/children")) {
                            strncpy(out[idx].key, key->valuestring, sizeof(out[idx].key) - 1);
                        } else {
                            snprintf(out[idx].key, sizeof(out[idx].key), "%s/children", key->valuestring);
                        }
                    }
                    if (title && cJSON_IsString(title)) strncpy(out[idx].title, title->valuestring, sizeof(out[idx].title) - 1);
                    if (summary && cJSON_IsString(summary)) strncpy(out[idx].summary, summary->valuestring, sizeof(out[idx].summary) - 1);
                    if (thumb && cJSON_IsString(thumb)) strncpy(out[idx].thumb, thumb->valuestring, sizeof(out[idx].thumb) - 1);
                    
                    items_parsed++;
                }
            }
            if (out_total && *out_total == 0) *out_total = items_parsed;
        }
        cJSON_Delete(json);
    }
    
    free(response);
    LOG_INFO("Fetched artists page start=%d count=%d -> parsed %d (total %d)", start, count, items_parsed, out_total ? *out_total : 0);
    return items_parsed;
}

int plex_api_get_artists(const char* library_key, PlexArtist* out, int max) {
    int total = 0;
    return plex_api_get_artists_page(library_key, out, 0, max, &total);
}

int plex_api_get_albums_page(const char* artist_key, PlexAlbum* out, int start, int count, int* out_total) {
    if (out_total) *out_total = 0;
    if (!artist_key || !out) return 0;
    
    char endpoint[512];
    if (strstr(artist_key, "?")) {
        snprintf(endpoint, sizeof(endpoint), "%s&X-Plex-Container-Start=%d&X-Plex-Container-Size=%d", artist_key, start, count);
    } else {
        snprintf(endpoint, sizeof(endpoint), "%s?X-Plex-Container-Start=%d&X-Plex-Container-Size=%d", artist_key, start, count);
    }
    
    char* response = NULL;
    if (!plex_http_get(endpoint, &response)) {
        return 0;
    }
    
    int items_parsed = 0;
    cJSON* json = cJSON_Parse(response);
    if (json) {
        cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
        if (container) {
            cJSON* totalSize = cJSON_GetObjectItem(container, "totalSize");
            if (totalSize && cJSON_IsNumber(totalSize) && out_total) {
                *out_total = totalSize->valueint;
            }
            
            cJSON* metadata = cJSON_GetObjectItem(container, "Metadata");
            if (metadata && cJSON_IsArray(metadata)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, metadata) {
                    int idx = start + items_parsed;
                    if (idx >= PLEX_MAX_ITEMS) break;
                    memset(&out[idx], 0, sizeof(PlexAlbum));
                    
                    cJSON* ratingKey = cJSON_GetObjectItem(item, "ratingKey");
                    cJSON* key = cJSON_GetObjectItem(item, "key");
                    cJSON* title = cJSON_GetObjectItem(item, "title");
                    cJSON* parentTitle = cJSON_GetObjectItem(item, "parentTitle");
                    cJSON* thumb = cJSON_GetObjectItem(item, "thumb");
                    cJSON* year = cJSON_GetObjectItem(item, "year");
                    cJSON* leafCount = cJSON_GetObjectItem(item, "leafCount");
                    
                    if (ratingKey) {
                        if (cJSON_IsString(ratingKey) && ratingKey->valuestring) strncpy(out[idx].rating_key, ratingKey->valuestring, sizeof(out[idx].rating_key) - 1);
                        else if (cJSON_IsNumber(ratingKey)) snprintf(out[idx].rating_key, sizeof(out[idx].rating_key), "%d", ratingKey->valueint);
                    }
                    if (key && cJSON_IsString(key)) {
                        if (strstr(key->valuestring, "/children")) {
                            strncpy(out[idx].key, key->valuestring, sizeof(out[idx].key) - 1);
                        } else {
                            snprintf(out[idx].key, sizeof(out[idx].key), "%s/children", key->valuestring);
                        }
                    }
                    if (title && cJSON_IsString(title)) strncpy(out[idx].title, title->valuestring, sizeof(out[idx].title) - 1);
                    if (parentTitle && cJSON_IsString(parentTitle)) strncpy(out[idx].parent_title, parentTitle->valuestring, sizeof(out[idx].parent_title) - 1);
                    if (thumb && cJSON_IsString(thumb)) strncpy(out[idx].thumb, thumb->valuestring, sizeof(out[idx].thumb) - 1);
                    if (year && cJSON_IsNumber(year)) out[idx].year = year->valueint;
                    if (leafCount && cJSON_IsNumber(leafCount)) out[idx].leaf_count = leafCount->valueint;
                    
                    items_parsed++;
                }
            }
            if (out_total && *out_total == 0) *out_total = items_parsed;
        }
        cJSON_Delete(json);
    }
    
    free(response);
    LOG_INFO("Fetched albums page start=%d count=%d -> parsed %d (total %d)", start, count, items_parsed, out_total ? *out_total : 0);
    return items_parsed;
}

int plex_api_get_albums(const char* artist_key, PlexAlbum* out, int max) {
    int total = 0;
    return plex_api_get_albums_page(artist_key, out, 0, max, &total);
}

static int parse_tracks_from_json(const char* response, PlexTrack* out, int start, int max, int* out_total) {
    if (!response || !out) return 0;
    int items_parsed = 0;
    cJSON* json = cJSON_Parse(response);
    if (json) {
        cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
        if (container) {
            cJSON* totalSize = cJSON_GetObjectItem(container, "totalSize");
            if (totalSize && cJSON_IsNumber(totalSize) && out_total) {
                *out_total = totalSize->valueint;
            }
            
            cJSON* metadata = cJSON_GetObjectItem(container, "Metadata");
            if (!metadata) metadata = cJSON_GetObjectItem(container, "Track");
            if (metadata && cJSON_IsArray(metadata)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, metadata) {
                    int idx = start + items_parsed;
                    if (idx >= PLEX_MAX_ITEMS) break;
                    memset(&out[idx], 0, sizeof(PlexTrack));
                    
                    cJSON* ratingKey = cJSON_GetObjectItem(item, "ratingKey");
                    cJSON* key = cJSON_GetObjectItem(item, "key");
                    cJSON* title = cJSON_GetObjectItem(item, "title");
                    cJSON* grandparentTitle = cJSON_GetObjectItem(item, "grandparentTitle");
                    cJSON* parentTitle = cJSON_GetObjectItem(item, "parentTitle");
                    cJSON* thumb = cJSON_GetObjectItem(item, "thumb");
                    cJSON* duration = cJSON_GetObjectItem(item, "duration");
                    cJSON* index = cJSON_GetObjectItem(item, "index");
                    cJSON* userRating = cJSON_GetObjectItem(item, "userRating"); // 0.0-10.0, omitted entirely if the track has never been rated
                    
                    if (ratingKey) {
                        if (cJSON_IsString(ratingKey) && ratingKey->valuestring) {
                            strncpy(out[idx].rating_key, ratingKey->valuestring, sizeof(out[idx].rating_key) - 1);
                        } else if (cJSON_IsNumber(ratingKey)) {
                            snprintf(out[idx].rating_key, sizeof(out[idx].rating_key), "%d", ratingKey->valueint);
                        }
                    }
                    if (out[idx].rating_key[0] == '\0' && key && cJSON_IsString(key)) {
                        const char* p = strrchr(key->valuestring, '/');
                        if (p && strlen(p + 1) > 0) {
                            strncpy(out[idx].rating_key, p + 1, sizeof(out[idx].rating_key) - 1);
                        }
                    }
                    if (title && cJSON_IsString(title)) strncpy(out[idx].title, title->valuestring, sizeof(out[idx].title) - 1);
                    if (grandparentTitle && cJSON_IsString(grandparentTitle)) strncpy(out[idx].grandparent_title, grandparentTitle->valuestring, sizeof(out[idx].grandparent_title) - 1);
                    if (parentTitle && cJSON_IsString(parentTitle)) strncpy(out[idx].parent_title, parentTitle->valuestring, sizeof(out[idx].parent_title) - 1);
                    if (thumb && cJSON_IsString(thumb)) strncpy(out[idx].thumb, thumb->valuestring, sizeof(out[idx].thumb) - 1);
                    if (duration && cJSON_IsNumber(duration)) out[idx].duration = duration->valueint;
                    if (index && cJSON_IsNumber(index)) out[idx].index = index->valueint;
                    if (userRating && cJSON_IsNumber(userRating)) out[idx].user_rating = (float)userRating->valuedouble;

                    cJSON* media = cJSON_GetObjectItem(item, "Media");
                    if (media && cJSON_IsArray(media)) {
                        cJSON* media_item = cJSON_GetArrayItem(media, 0);
                        if (media_item) {
                            cJSON* codec = cJSON_GetObjectItem(media_item, "audioCodec");
                            cJSON* bitrate = cJSON_GetObjectItem(media_item, "bitrate");
                            cJSON* bitDepth = cJSON_GetObjectItem(media_item, "bitDepth");
                            cJSON* samplingRate = cJSON_GetObjectItem(media_item, "samplingRate");
                            
                            if (codec && cJSON_IsString(codec)) strncpy(out[idx].audio_codec, codec->valuestring, sizeof(out[idx].audio_codec) - 1);
                            if (bitrate && cJSON_IsNumber(bitrate)) out[idx].bitrate = bitrate->valueint;
                            if (bitDepth && cJSON_IsNumber(bitDepth)) out[idx].bit_depth = bitDepth->valueint;
                            if (samplingRate && cJSON_IsNumber(samplingRate)) out[idx].sampling_rate = samplingRate->valueint;
                            
                            cJSON* part = cJSON_GetObjectItem(media_item, "Part");
                            if (part && cJSON_IsArray(part)) {
                                cJSON* part_item = cJSON_GetArrayItem(part, 0);
                                if (part_item) {
                                    cJSON* part_key = cJSON_GetObjectItem(part_item, "key");
                                    if (part_key && cJSON_IsString(part_key)) {
                                        strncpy(out[idx].part_key, part_key->valuestring, sizeof(out[idx].part_key) - 1);
                                    }
                                }
                            }
                        }
                    }
                    
                    if (out[idx].part_key[0] != '\0' || out[idx].rating_key[0] != '\0') {
                        items_parsed++;
                    }
                }
            }
            if (out_total && *out_total == 0) *out_total = items_parsed;
        }
        cJSON_Delete(json);
    }
    return items_parsed;
}

int plex_api_get_tracks_page(const char* album_key, PlexTrack* out, int start, int count, int* out_total) {
    if (out_total) *out_total = 0;
    if (!album_key || !out) return 0;
    
    char endpoint[512];
    if (strstr(album_key, "?")) {
        snprintf(endpoint, sizeof(endpoint), "%s&X-Plex-Container-Start=%d&X-Plex-Container-Size=%d", album_key, start, count);
    } else {
        snprintf(endpoint, sizeof(endpoint), "%s?X-Plex-Container-Start=%d&X-Plex-Container-Size=%d", album_key, start, count);
    }
    
    char* response = NULL;
    if (!plex_http_get(endpoint, &response)) {
        return 0;
    }
    int items_parsed = parse_tracks_from_json(response, out, start, count, out_total);
    free(response);
    LOG_INFO("Fetched tracks page start=%d count=%d -> parsed %d (total %d)", start, count, items_parsed, out_total ? *out_total : 0);
    return items_parsed;
}

int plex_api_get_tracks(const char* album_key, PlexTrack* out, int max) {
    int total = 0;
    return plex_api_get_tracks_page(album_key, out, 0, max, &total);
}

int plex_api_get_playlists(PlexPlaylist* out, int max) {
    char* response = NULL;
    if (!plex_http_get("/playlists?playlistType=audio", &response)) {
        return 0;
    }
    
    int count = 0;
    cJSON* json = cJSON_Parse(response);
    if (json) {
        cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
        if (container) {
            cJSON* metadata = cJSON_GetObjectItem(container, "Metadata");
            if (metadata && cJSON_IsArray(metadata)) {
                cJSON* item = NULL;
                cJSON_ArrayForEach(item, metadata) {
                    if (count >= max) break;
                    
                    cJSON* ratingKey = cJSON_GetObjectItem(item, "ratingKey");
                    cJSON* key = cJSON_GetObjectItem(item, "key");
                    cJSON* title = cJSON_GetObjectItem(item, "title");
                    cJSON* duration = cJSON_GetObjectItem(item, "duration");
                    cJSON* leafCount = cJSON_GetObjectItem(item, "leafCount");
                    
                    if (ratingKey && cJSON_IsString(ratingKey)) strncpy(out[count].rating_key, ratingKey->valuestring, sizeof(out[count].rating_key) - 1);
                    if (key && cJSON_IsString(key)) {
                        if (strstr(key->valuestring, "/items")) {
                            strncpy(out[count].key, key->valuestring, sizeof(out[count].key) - 1);
                        } else {
                            snprintf(out[count].key, sizeof(out[count].key), "%s/items", key->valuestring);
                        }
                    }
                    if (title && cJSON_IsString(title)) strncpy(out[count].title, title->valuestring, sizeof(out[count].title) - 1);
                    if (duration && cJSON_IsNumber(duration)) out[count].duration = duration->valueint;
                    if (leafCount && cJSON_IsNumber(leafCount)) out[count].leaf_count = leafCount->valueint;
                    
                    count++;
                }
            }
        }
        cJSON_Delete(json);
    }
    
    free(response);
    return count;
}

int plex_api_get_playlist_tracks(const char* playlist_key, PlexTrack* out, int max) {
    return plex_api_get_tracks(playlist_key, out, max);
}

int plex_api_search_tracks(const char* query, PlexTrack* out, int max) {
    if (!query || !query[0]) return 0;
    
    char endpoint[512] = "/search?type=10&query=";
    url_encode_cat(endpoint, sizeof(endpoint), query);
    
    char* response = NULL;
    if (!plex_http_get(endpoint, &response)) {
        return 0;
    }
    
    int count = parse_tracks_from_json(response, out, 0, max, NULL);
    free(response);
    return count;
}

int plex_api_get_recently_added(PlexTrack* out, int max) {
    char* response = NULL;
    if (!plex_http_get("/library/recentlyAdded?type=10", &response)) {
        return 0;
    }
    
    int count = parse_tracks_from_json(response, out, 0, max, NULL);
    free(response);
    return count;
}

static bool plex_http_get_binary(const char* url, const char* token, u8** response_out, size_t* size_out) {
    if (!url || !url[0] || !response_out || !size_out) return false;
    *response_out = NULL;
    *size_out = 0;
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    HttpBuffer chunk;
    chunk.data = malloc(1);
    chunk.size = 0;
    chunk.capacity = 1;
    if (chunk.data) chunk.data[0] = 0;
    
    struct curl_slist* headers = NULL;
    char token_header[256];
    snprintf(token_header, sizeof(token_header), "X-Plex-Token: %s", token ? token : s_auth_token);
    headers = curl_slist_append(headers, token_header);
    headers = curl_slist_append(headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    
    CURLcode res = perform_blocking_request(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    if (res == CURLE_OK && http_code == 200 && chunk.data && chunk.size > 0) {
        *response_out = (u8*)chunk.data;
        *size_out = chunk.size;
        LOG_INFO("Fetched binary image (%d bytes) from %s", (int)chunk.size, url);
        return true;
    }
    
    if (chunk.data) free(chunk.data);
    LOG_WARN("plex_http_get_binary failed: res=%d, http=%ld", res, http_code);
    return false;
}

// Shared by plex_api_get_transcode_url() (offset_sec=0) and
// plex_api_get_seek_url() (offset_sec = where the user scrubbed to) - a seek
// is just a fresh transcode reload starting from a different point in the
// track, using Plex's own "offset" param so the server does the real seeking
// (accurate regardless of codec/bitrate, unlike guessing a byte offset).
static bool build_transcode_url(const PlexTrack* track, int offset_sec, char* url_out, size_t url_max) {
    if (!s_initialized || !track) return false;

    char raw_path[512];
    if (track->rating_key[0] != '\0') {
        if (strncmp(track->rating_key, "/library/metadata/", 18) == 0) {
            snprintf(raw_path, sizeof(raw_path), "%s", track->rating_key);
        } else if (track->rating_key[0] == '/') {
            snprintf(raw_path, sizeof(raw_path), "%s", track->rating_key);
        } else {
            snprintf(raw_path, sizeof(raw_path), "/library/metadata/%s", track->rating_key);
        }
    } else if (track->part_key[0] != '\0') {
        snprintf(raw_path, sizeof(raw_path), "%s", track->part_key);
    } else {
        return false;
    }
    
    // Build the full internal URL that the transcode endpoint expects
    // (e.g. "http://127.0.0.1:32400/library/metadata/12345")
    char full_path[PLEX_MAX_URL];
    snprintf(full_path, sizeof(full_path), "%s%s", s_server_url, raw_path);
    
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    
    char* encoded_path = curl_easy_escape(curl, full_path, 0);
    if (!encoded_path) {
        curl_easy_cleanup(curl);
        return false;
    }
    
    int bitrate = quality_tier_to_bitrate(s_quality_tier);
    if (bitrate <= 0) {
        // QUALITY_FLAC_DIRECT has no bitrate cap (it means "don't transcode at all"),
        // but we only get here when a transcode is actually unavoidable (e.g. non-N3DS
        // device or non-FLAC file). Sending audioBitrate=0 makes Plex reject the request
        // with a 400, so fall back to the highest MP3 quality instead.
        bitrate = 320;
    }

    // Give this request its own transcode session id instead of reusing a single
    // hardcoded value for every track. PMS keys concurrent transcode sessions by
    // this id, and the app runs two transcodes concurrently by design (the
    // current track playing while the next track is prefetched) - sharing one
    // session id between them makes PMS treat the prefetch as re-targeting the
    // playing track's session, which 400s both requests.
    const char* rkey_tail = strrchr(track->rating_key, '/');
    rkey_tail = (rkey_tail && rkey_tail[1]) ? rkey_tail + 1 : track->rating_key;
    char session_id[PLEX_MAX_STR + 32];
    snprintf(session_id, sizeof(session_id), PLEX_CLIENT_ID "-%s", rkey_tail[0] ? rkey_tail : "0");

    // Auth and client identity go on the request as X-Plex-* headers (set by
    // audio_player.c when it fetches this URL), not as query params here -
    // Plex's transcode decision endpoint 400s if the same identity is asserted
    // both ways and they don't line up exactly.
    snprintf(url_out, url_max,
        "%s/music/:/transcode/universal/start.mp3?path=%s&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0&directStreamAudio=0&audioCodec=mp3&container=mp3&maxAudioBitrate=%d&audioBitrate=%d&offset=%d&session=%s",
        s_server_url, encoded_path, bitrate, bitrate, offset_sec > 0 ? offset_sec : 0, session_id);

    curl_free(encoded_path);
    curl_easy_cleanup(curl);

    LOG_INFO("Built Transcode URL (rkey='%s', offset=%ds): %s", track->rating_key, offset_sec, url_out);
    return true;
}

bool plex_api_get_transcode_url(const PlexTrack* track, char* url_out, size_t url_max) {
    return build_transcode_url(track, 0, url_out, url_max);
}

bool plex_api_get_seek_url(const PlexTrack* track, int seek_ms, char* url_out, size_t url_max) {
    return build_transcode_url(track, seek_ms / 1000, url_out, url_max);
}

bool plex_api_get_stream_url(const PlexTrack* track, char* url_out, size_t url_max) {
    if (!s_initialized || !track) return false;
    
    bool is_n3ds = false;
    APT_CheckNew3DS(&is_n3ds);
    
    int tier_bitrate = quality_tier_to_bitrate(s_quality_tier);
    
    // N3DS FLAC direct streaming: only if quality tier allows it, AND the
    // source's native sample rate is one the 3DS's DSP handles reliably.
    // The DSP's actual internal mixing rate is a fixed ~32728Hz
    // (NDSP_SAMPLE_RATE = SYSCLOCK_SOC/512); ndspChnSetRate() has it resample
    // from whatever rate we claim. That works fine for standard rates
    // (44100/48000, both well short of 2x the native rate), but hi-res
    // masters (88.2/96/176.4/192kHz - not rare on well-tagged FLAC libraries)
    // push the resampling ratio far enough to audibly speed up and pitch-shift
    // playback. Anything above 48000Hz falls through to a transcode instead,
    // which normalizes to a safe rate automatically.
    if (s_quality_tier == QUALITY_FLAC_DIRECT && is_n3ds &&
        track->part_key[0] != '\0' &&
        (strstr(track->part_key, ".flac") || strstr(track->part_key, ".FLAC")) &&
        (track->sampling_rate <= 0 || track->sampling_rate <= 48000)) {
        snprintf(url_out, url_max, "%s%s", s_server_url, track->part_key);
        LOG_INFO("⚡ N3DS FLAC Direct Stream (tier=%s) for %s: %s",
                 plex_api_get_quality_label(s_quality_tier), track->title, url_out);
        return true;
    }
    
    // Direct MP3 stream: only if the file bitrate fits within our quality tier
    if (track->part_key[0] != '\0' && 
        (strstr(track->part_key, ".mp3") || strstr(track->part_key, ".MP3"))) {
        if (track->bitrate <= 0 || track->bitrate <= tier_bitrate) {
            // File bitrate is within our tier, direct stream is OK
            snprintf(url_out, url_max, "%s%s", s_server_url, track->part_key);
            LOG_INFO("Direct MP3 stream (tier=%s, file=%dkbps) for %s: %s",
                     plex_api_get_quality_label(s_quality_tier), track->bitrate, track->title, url_out);
            return true;
        }
        // File bitrate exceeds our tier - fall through to transcode
        LOG_INFO("MP3 file bitrate %dk exceeds tier %s - will transcode", 
                 track->bitrate, plex_api_get_quality_label(s_quality_tier));
    }
    
    // Everything else: transcode to MP3 at current tier bitrate
    if (track->rating_key[0] != '\0') {
        return plex_api_get_transcode_url(track, url_out, url_max);
    }
    
    // Fallback: direct stream the part_key as-is
    snprintf(url_out, url_max, "%s%s", s_server_url, track->part_key);
    return true;
}

void plex_api_report_timeline(const char* rating_key, const char* state, int time_ms, int duration_ms) {
    if (!s_initialized || !rating_key || !rating_key[0]) return;
    
    const char* clean_rkey = rating_key;
    const char* p = strrchr(rating_key, '/');
    if (p && strlen(p + 1) > 0) {
        clean_rkey = p + 1;
    }
    
    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint), 
        "/:/timeline?ratingKey=%s&key=%%2Flibrary%%2Fmetadata%%2F%s&state=%s&time=%d&duration=%d&hasSample=0",
        clean_rkey, clean_rkey, state ? state : "playing", time_ms > 0 ? time_ms : 0, duration_ms > 0 ? duration_ms : 0);
        
    char* resp = NULL;
    plex_http_get(endpoint, &resp);
    if (resp) free(resp);
}

// --- Async lyrics fetch --------------------------------------------------
// Same two-request flow as plex_api_get_lyrics() (metadata -> find the lrc
// stream's key -> fetch its content), but pumped non-blockingly via its own
// curl_multi handle instead of curl_easy_perform(), so callers can start it
// right when a track begins playing without freezing the frame.

// Defined later in this file (next to plex_api_get_lyrics(), which shares
// it); forward-declared here since this section runs before that point.
int parse_lyrics_stream_response(const char* response, PlexLyricLine* out, int max);

typedef enum {
    LYR_ASYNC_IDLE,
    LYR_ASYNC_FETCHING_METADATA,
    LYR_ASYNC_FETCHING_LYRICS,
    LYR_ASYNC_DONE
} LyricsAsyncState;

#define PLEX_LYRICS_ASYNC_MAX 64

static LyricsAsyncState s_lyr_state = LYR_ASYNC_IDLE;
static CURLM* s_lyr_multi = NULL;
static CURL* s_lyr_easy = NULL;
static struct curl_slist* s_lyr_headers = NULL;
static HttpBuffer s_lyr_buf = {0};
static char s_lyr_rating_key[PLEX_MAX_STR] = {0};
static char s_lyr_stream_key[512] = {0};
static PlexLyricLine s_lyr_result[PLEX_LYRICS_ASYNC_MAX];
static int s_lyr_result_count = 0;

static void lyr_async_cleanup_request(void) {
    if (s_lyr_multi && s_lyr_easy) {
        curl_multi_remove_handle(s_lyr_multi, s_lyr_easy);
        curl_easy_cleanup(s_lyr_easy);
        s_lyr_easy = NULL;
    }
    if (s_lyr_headers) {
        curl_slist_free_all(s_lyr_headers);
        s_lyr_headers = NULL;
    }
    if (s_lyr_buf.data) {
        free(s_lyr_buf.data);
        s_lyr_buf.data = NULL;
    }
    s_lyr_buf.size = 0;
    s_lyr_buf.capacity = 0;
}

static bool lyr_async_start_request(const char* endpoint) {
    if (!s_lyr_multi) s_lyr_multi = curl_multi_init();
    s_lyr_easy = curl_easy_init();
    if (!s_lyr_easy) return false;

    char url[PLEX_MAX_URL];
    snprintf(url, sizeof(url), "%s%s", s_server_url, endpoint);

    s_lyr_buf.data = malloc(1);
    s_lyr_buf.size = 0;
    s_lyr_buf.capacity = 1;
    if (s_lyr_buf.data) s_lyr_buf.data[0] = 0;

    s_lyr_headers = curl_slist_append(NULL, "Accept: application/json");
    char token_header[256];
    snprintf(token_header, sizeof(token_header), "X-Plex-Token: %s", s_auth_token);
    s_lyr_headers = curl_slist_append(s_lyr_headers, token_header);
    s_lyr_headers = curl_slist_append(s_lyr_headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    s_lyr_headers = curl_slist_append(s_lyr_headers, "X-Plex-Product: " PLEX_PRODUCT);

    curl_easy_setopt(s_lyr_easy, CURLOPT_URL, url);
    curl_easy_setopt(s_lyr_easy, CURLOPT_HTTPHEADER, s_lyr_headers);
    curl_easy_setopt(s_lyr_easy, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(s_lyr_easy, CURLOPT_WRITEDATA, (void*)&s_lyr_buf);
    curl_easy_setopt(s_lyr_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_lyr_easy, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(s_lyr_easy, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(s_lyr_easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s_lyr_easy, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_multi_add_handle(s_lyr_multi, s_lyr_easy);
    return true;
}

static void lyr_async_finish_with_no_lyrics(void) {
    s_lyr_result[0].time_ms = 0;
    strncpy(s_lyr_result[0].text, "No time-synced lyrics available", sizeof(s_lyr_result[0].text) - 1);
    s_lyr_result[0].text[sizeof(s_lyr_result[0].text) - 1] = '\0';
    s_lyr_result_count = 1;
    s_lyr_state = LYR_ASYNC_DONE;
}

void plex_api_lyrics_async_start(const char* rating_key) {
    lyr_async_cleanup_request();
    s_lyr_state = LYR_ASYNC_IDLE;
    s_lyr_result_count = 0;
    s_lyr_stream_key[0] = '\0';

    if (!s_initialized || !rating_key || !rating_key[0]) return;
    snprintf(s_lyr_rating_key, sizeof(s_lyr_rating_key), "%s", rating_key);

    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/library/metadata/%s", rating_key);
    if (lyr_async_start_request(endpoint)) {
        s_lyr_state = LYR_ASYNC_FETCHING_METADATA;
    }
}

void plex_api_lyrics_async_update(void) {
    if (s_lyr_state == LYR_ASYNC_IDLE || s_lyr_state == LYR_ASYNC_DONE) return;
    if (!s_lyr_multi || !s_lyr_easy) return;

    int running = 0;
    CURLMcode mres = curl_multi_perform(s_lyr_multi, &running);
    if (mres == CURLM_OK && running > 0) return; // still in flight, check again next frame

    long http_code = 0;
    curl_easy_getinfo(s_lyr_easy, CURLINFO_RESPONSE_CODE, &http_code);
    bool ok = (mres == CURLM_OK && http_code == 200 && s_lyr_buf.data && s_lyr_buf.size > 0);

    if (s_lyr_state == LYR_ASYNC_FETCHING_METADATA) {
        char fallback_key[512] = "";
        if (ok) {
            cJSON* json = cJSON_Parse(s_lyr_buf.data);
            if (json) {
                cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
                cJSON* metadata = container ? cJSON_GetObjectItem(container, "Metadata") : NULL;
                cJSON* item = (metadata && cJSON_IsArray(metadata)) ? cJSON_GetArrayItem(metadata, 0) : NULL;
                cJSON* media = item ? cJSON_GetObjectItem(item, "Media") : NULL;
                cJSON* media_item = (media && cJSON_IsArray(media)) ? cJSON_GetArrayItem(media, 0) : NULL;
                cJSON* part = media_item ? cJSON_GetObjectItem(media_item, "Part") : NULL;
                cJSON* part_item = (part && cJSON_IsArray(part)) ? cJSON_GetArrayItem(part, 0) : NULL;
                cJSON* stream = part_item ? cJSON_GetObjectItem(part_item, "Stream") : NULL;
                if (stream && cJSON_IsArray(stream)) {
                    cJSON* s_item = NULL;
                    cJSON_ArrayForEach(s_item, stream) {
                        cJSON* streamType = cJSON_GetObjectItem(s_item, "streamType");
                        cJSON* key = cJSON_GetObjectItem(s_item, "key");
                        if (streamType && cJSON_IsNumber(streamType) && streamType->valueint == 4 &&
                            key && cJSON_IsString(key)) {
                            if (fallback_key[0] == '\0') {
                                strncpy(fallback_key, key->valuestring, sizeof(fallback_key) - 1);
                            }
                            cJSON* format = cJSON_GetObjectItem(s_item, "format");
                            if (format && cJSON_IsString(format) && strcmp(format->valuestring, "lrc") == 0) {
                                strncpy(s_lyr_stream_key, key->valuestring, sizeof(s_lyr_stream_key) - 1);
                                break;
                            }
                        }
                    }
                }
                cJSON_Delete(json);
            }
        }
        lyr_async_cleanup_request();

        if (s_lyr_stream_key[0] == '\0' && fallback_key[0] != '\0') {
            strncpy(s_lyr_stream_key, fallback_key, sizeof(s_lyr_stream_key) - 1);
        }
        if (s_lyr_stream_key[0] == '\0') {
            snprintf(s_lyr_stream_key, sizeof(s_lyr_stream_key), "/library/metadata/%s/subtitles", s_lyr_rating_key);
        }

        if (lyr_async_start_request(s_lyr_stream_key)) {
            s_lyr_state = LYR_ASYNC_FETCHING_LYRICS;
        } else {
            lyr_async_finish_with_no_lyrics();
        }
        return;
    }

    if (s_lyr_state == LYR_ASYNC_FETCHING_LYRICS) {
        int count = ok ? parse_lyrics_stream_response(s_lyr_buf.data, s_lyr_result, PLEX_LYRICS_ASYNC_MAX) : 0;
        lyr_async_cleanup_request();

        if (count == 0) {
            lyr_async_finish_with_no_lyrics();
        } else {
            s_lyr_result_count = count;
            s_lyr_state = LYR_ASYNC_DONE;
        }
        LOG_INFO("Loaded %d lyric lines (async) for track %s", s_lyr_result_count, s_lyr_rating_key);
    }
}

bool plex_api_lyrics_async_is_done(void) {
    return s_lyr_state == LYR_ASYNC_DONE;
}

int plex_api_lyrics_async_take_result(PlexLyricLine* out, int max) {
    if (s_lyr_state != LYR_ASYNC_DONE || !out || max <= 0) return 0;
    int n = s_lyr_result_count < max ? s_lyr_result_count : max;
    memcpy(out, s_lyr_result, (size_t)n * sizeof(PlexLyricLine));
    s_lyr_state = LYR_ASYNC_IDLE; // consumed
    return n;
}

// --- Async (fire-and-forget) timeline reporting --------------------------

static CURLM* s_tl_multi = NULL;
static CURL* s_tl_easy = NULL;
static struct curl_slist* s_tl_headers = NULL;
static HttpBuffer s_tl_buf = {0};
static bool s_tl_active = false;

static void tl_async_cleanup(void) {
    if (s_tl_multi && s_tl_easy) {
        curl_multi_remove_handle(s_tl_multi, s_tl_easy);
        curl_easy_cleanup(s_tl_easy);
        s_tl_easy = NULL;
    }
    if (s_tl_headers) {
        curl_slist_free_all(s_tl_headers);
        s_tl_headers = NULL;
    }
    if (s_tl_buf.data) {
        free(s_tl_buf.data);
        s_tl_buf.data = NULL;
    }
    s_tl_buf.size = 0;
    s_tl_buf.capacity = 0;
    s_tl_active = false;
}

void plex_api_report_timeline_async(const char* rating_key, const char* state, int time_ms, int duration_ms) {
    if (!s_initialized || !rating_key || !rating_key[0]) return;

    // A newer report replaces whatever's still in flight rather than queuing.
    tl_async_cleanup();

    const char* clean_rkey = rating_key;
    const char* p = strrchr(rating_key, '/');
    if (p && strlen(p + 1) > 0) clean_rkey = p + 1;

    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint),
        "/:/timeline?ratingKey=%s&key=%%2Flibrary%%2Fmetadata%%2F%s&state=%s&time=%d&duration=%d&hasSample=0",
        clean_rkey, clean_rkey, state ? state : "playing", time_ms > 0 ? time_ms : 0, duration_ms > 0 ? duration_ms : 0);

    char url[PLEX_MAX_URL];
    snprintf(url, sizeof(url), "%s%s", s_server_url, endpoint);

    if (!s_tl_multi) s_tl_multi = curl_multi_init();
    s_tl_easy = curl_easy_init();
    if (!s_tl_easy) return;

    s_tl_buf.data = malloc(1);
    s_tl_buf.size = 0;
    s_tl_buf.capacity = 1;
    if (s_tl_buf.data) s_tl_buf.data[0] = 0;

    s_tl_headers = curl_slist_append(NULL, "Accept: application/json");
    char token_header[256];
    snprintf(token_header, sizeof(token_header), "X-Plex-Token: %s", s_auth_token);
    s_tl_headers = curl_slist_append(s_tl_headers, token_header);
    s_tl_headers = curl_slist_append(s_tl_headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    s_tl_headers = curl_slist_append(s_tl_headers, "X-Plex-Product: " PLEX_PRODUCT);

    curl_easy_setopt(s_tl_easy, CURLOPT_URL, url);
    curl_easy_setopt(s_tl_easy, CURLOPT_HTTPHEADER, s_tl_headers);
    curl_easy_setopt(s_tl_easy, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(s_tl_easy, CURLOPT_WRITEDATA, (void*)&s_tl_buf);
    curl_easy_setopt(s_tl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_tl_easy, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(s_tl_easy, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(s_tl_easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s_tl_easy, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_multi_add_handle(s_tl_multi, s_tl_easy);
    s_tl_active = true;
}

void plex_api_timeline_async_update(void) {
    if (!s_tl_active || !s_tl_multi || !s_tl_easy) return;
    int running = 0;
    CURLMcode mres = curl_multi_perform(s_tl_multi, &running);
    if (mres == CURLM_OK && running > 0) return;
    tl_async_cleanup(); // done (success or failure - fire-and-forget either way)
}

void plex_api_rate_track_async(const char* rating_key, float rating_10) {
    if (!s_initialized || !rating_key || !rating_key[0]) return;

    // Shares the timeline-report request slot above - see this function's
    // doc comment in plex_api.h for why that's fine.
    tl_async_cleanup();

    const char* clean_rkey = rating_key;
    const char* p = strrchr(rating_key, '/');
    if (p && strlen(p + 1) > 0) clean_rkey = p + 1;

    int rating_int = (int)(rating_10 >= 0.0f ? rating_10 + 0.5f : rating_10 - 0.5f); // round to nearest whole point
    if (rating_int > 10) rating_int = 10;
    if (rating_int < -1) rating_int = -1;

    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/:/rate?identifier=com.plexapp.plugins.library&key=%s&rating=%d",
             clean_rkey, rating_int);

    char url[PLEX_MAX_URL];
    snprintf(url, sizeof(url), "%s%s", s_server_url, endpoint);

    if (!s_tl_multi) s_tl_multi = curl_multi_init();
    s_tl_easy = curl_easy_init();
    if (!s_tl_easy) return;

    s_tl_buf.data = malloc(1);
    s_tl_buf.size = 0;
    s_tl_buf.capacity = 1;
    if (s_tl_buf.data) s_tl_buf.data[0] = 0;

    s_tl_headers = curl_slist_append(NULL, "Accept: application/json");
    char token_header[256];
    snprintf(token_header, sizeof(token_header), "X-Plex-Token: %s", s_auth_token);
    s_tl_headers = curl_slist_append(s_tl_headers, token_header);
    s_tl_headers = curl_slist_append(s_tl_headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    s_tl_headers = curl_slist_append(s_tl_headers, "X-Plex-Product: " PLEX_PRODUCT);

    curl_easy_setopt(s_tl_easy, CURLOPT_URL, url);
    curl_easy_setopt(s_tl_easy, CURLOPT_HTTPHEADER, s_tl_headers);
    curl_easy_setopt(s_tl_easy, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(s_tl_easy, CURLOPT_WRITEDATA, (void*)&s_tl_buf);
    curl_easy_setopt(s_tl_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_tl_easy, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(s_tl_easy, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(s_tl_easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s_tl_easy, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_multi_add_handle(s_tl_multi, s_tl_easy);
    s_tl_active = true;

    LOG_INFO("Rating track %s: %d/10 (async)", clean_rkey, rating_int);
}

// --- Sonic Analysis (loudness) async fetch --------------------------------
// Two-stage, same shape as the lyrics async fetch above: first resolve the
// track's audio Stream (for its gain/loudness/lra scalars and its stream id,
// via /library/metadata/{ratingKey}), then fetch that stream's short-term
// loudness curve (/library/streams/{id}/loudness - plain text, one dB value
// per line, one entry per 100ms per developer.plex.tv's
// libraryGetStreamsStreamLoudness) and keep only the requested end of it.

// Parses a /library/streams/{id}/loudness response (plain text, one float
// per line) into out, keeping at most `max` samples. If want_tail, keeps the
// LAST max samples seen (a sliding window, so the result ends up being
// whatever was closest to the end of the track regardless of how long the
// full response was); otherwise keeps the FIRST max samples and stops
// parsing as soon as it has enough. Returns the number of samples written.
// External linkage (not declared in the header) so the host test suite can
// exercise it directly against a captured fixture, same as
// parse_lyrics_stream_response().
int parse_loudness_curve_response(const char* text, float* out, int max, bool want_tail) {
    if (!text || !out || max <= 0) return 0;
    int n = 0; // valid samples currently in out[0..n-1], oldest -> newest
    const char* p = text;
    while (*p) {
        char* endptr = NULL;
        float v = strtof(p, &endptr);
        if (endptr == p) { p++; continue; } // not a number (e.g. stray whitespace/newline) - skip a char and keep scanning
        p = endptr;
        if (n < max) {
            out[n++] = v;
        } else if (want_tail) {
            memmove(out, out + 1, (size_t)(max - 1) * sizeof(float));
            out[max - 1] = v;
        } else {
            break; // head mode: already have enough, no need to keep parsing
        }
    }
    return n;
}

typedef enum {
    ANALYSIS_ASYNC_IDLE,
    ANALYSIS_ASYNC_FETCHING_METADATA,
    ANALYSIS_ASYNC_FETCHING_CURVE,
    ANALYSIS_ASYNC_DONE
} AnalysisAsyncState;

static AnalysisAsyncState s_an_state = ANALYSIS_ASYNC_IDLE;
static CURLM* s_an_multi = NULL;
static CURL* s_an_easy = NULL;
static struct curl_slist* s_an_headers = NULL;
static HttpBuffer s_an_buf = {0};
static bool s_an_want_tail = true;
static PlexTrackAnalysis s_an_result = {0};

static void an_async_cleanup_request(void) {
    if (s_an_multi && s_an_easy) {
        curl_multi_remove_handle(s_an_multi, s_an_easy);
        curl_easy_cleanup(s_an_easy);
        s_an_easy = NULL;
    }
    if (s_an_headers) {
        curl_slist_free_all(s_an_headers);
        s_an_headers = NULL;
    }
    if (s_an_buf.data) {
        free(s_an_buf.data);
        s_an_buf.data = NULL;
    }
    s_an_buf.size = 0;
    s_an_buf.capacity = 0;
}

static bool an_async_start_request(const char* endpoint) {
    if (!s_an_multi) s_an_multi = curl_multi_init();
    s_an_easy = curl_easy_init();
    if (!s_an_easy) return false;

    char url[PLEX_MAX_URL];
    snprintf(url, sizeof(url), "%s%s", s_server_url, endpoint);

    s_an_buf.data = malloc(1);
    s_an_buf.size = 0;
    s_an_buf.capacity = 1;
    if (s_an_buf.data) s_an_buf.data[0] = 0;

    s_an_headers = curl_slist_append(NULL, "Accept: application/json");
    char token_header[256];
    snprintf(token_header, sizeof(token_header), "X-Plex-Token: %s", s_auth_token);
    s_an_headers = curl_slist_append(s_an_headers, token_header);
    s_an_headers = curl_slist_append(s_an_headers, "X-Plex-Client-Identifier: " PLEX_CLIENT_ID);
    s_an_headers = curl_slist_append(s_an_headers, "X-Plex-Product: " PLEX_PRODUCT);

    curl_easy_setopt(s_an_easy, CURLOPT_URL, url);
    curl_easy_setopt(s_an_easy, CURLOPT_HTTPHEADER, s_an_headers);
    curl_easy_setopt(s_an_easy, CURLOPT_WRITEFUNCTION, http_write_cb);
    curl_easy_setopt(s_an_easy, CURLOPT_WRITEDATA, (void*)&s_an_buf);
    curl_easy_setopt(s_an_easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(s_an_easy, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(s_an_easy, CURLOPT_TIMEOUT, 12L);
    curl_easy_setopt(s_an_easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(s_an_easy, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_multi_add_handle(s_an_multi, s_an_easy);
    return true;
}

void plex_api_analysis_async_start(const char* rating_key, bool want_tail) {
    an_async_cleanup_request();
    s_an_state = ANALYSIS_ASYNC_IDLE;
    memset(&s_an_result, 0, sizeof(s_an_result));
    s_an_want_tail = want_tail;

    if (!s_initialized || !rating_key || !rating_key[0]) return;

    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/library/metadata/%s", rating_key);
    if (an_async_start_request(endpoint)) {
        s_an_state = ANALYSIS_ASYNC_FETCHING_METADATA;
    }
}

void plex_api_analysis_async_update(void) {
    if (s_an_state == ANALYSIS_ASYNC_IDLE || s_an_state == ANALYSIS_ASYNC_DONE) return;
    if (!s_an_multi || !s_an_easy) return;

    int running = 0;
    CURLMcode mres = curl_multi_perform(s_an_multi, &running);
    if (mres == CURLM_OK && running > 0) return; // still in flight, check again next frame

    long http_code = 0;
    curl_easy_getinfo(s_an_easy, CURLINFO_RESPONSE_CODE, &http_code);
    bool ok = (mres == CURLM_OK && http_code == 200 && s_an_buf.data && s_an_buf.size > 0);

    if (s_an_state == ANALYSIS_ASYNC_FETCHING_METADATA) {
        int stream_id = 0;
        if (ok) {
            cJSON* json = cJSON_Parse(s_an_buf.data);
            if (json) {
                cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
                cJSON* metadata = container ? cJSON_GetObjectItem(container, "Metadata") : NULL;
                cJSON* item = (metadata && cJSON_IsArray(metadata)) ? cJSON_GetArrayItem(metadata, 0) : NULL;
                cJSON* media = item ? cJSON_GetObjectItem(item, "Media") : NULL;
                cJSON* media_item = (media && cJSON_IsArray(media)) ? cJSON_GetArrayItem(media, 0) : NULL;
                cJSON* part = media_item ? cJSON_GetObjectItem(media_item, "Part") : NULL;
                cJSON* part_item = (part && cJSON_IsArray(part)) ? cJSON_GetArrayItem(part, 0) : NULL;
                cJSON* stream = part_item ? cJSON_GetObjectItem(part_item, "Stream") : NULL;
                cJSON* s_item = NULL;
                cJSON_ArrayForEach(s_item, stream) {
                    cJSON* streamType = cJSON_GetObjectItem(s_item, "streamType");
                    if (!streamType || !cJSON_IsNumber(streamType) || streamType->valueint != 2) continue; // 2 = audio
                    cJSON* id = cJSON_GetObjectItem(s_item, "id");
                    cJSON* gain = cJSON_GetObjectItem(s_item, "gain");
                    cJSON* loudness = cJSON_GetObjectItem(s_item, "loudness");
                    cJSON* lra = cJSON_GetObjectItem(s_item, "lra");
                    if (id && cJSON_IsNumber(id)) stream_id = id->valueint;
                    if (gain && cJSON_IsString(gain)) s_an_result.gain_db = strtof(gain->valuestring, NULL);
                    if (loudness && cJSON_IsString(loudness)) s_an_result.loudness_lufs = strtof(loudness->valuestring, NULL);
                    if (lra && cJSON_IsString(lra)) s_an_result.lra = strtof(lra->valuestring, NULL);
                    break;
                }
                cJSON_Delete(json);
            }
        }
        an_async_cleanup_request();

        if (stream_id <= 0) {
            // No audio Stream / no gain data - track was never analyzed (or
            // this server doesn't have Plex Pass). Report "no data" rather
            // than fetching a curve that can't exist either.
            s_an_state = ANALYSIS_ASYNC_DONE;
            return;
        }

        char curve_endpoint[128];
        snprintf(curve_endpoint, sizeof(curve_endpoint), "/library/streams/%d/loudness", stream_id);
        if (an_async_start_request(curve_endpoint)) {
            s_an_state = ANALYSIS_ASYNC_FETCHING_CURVE;
        } else {
            s_an_result.valid = true; // still have the scalar gain/loudness/lra even without a curve
            s_an_state = ANALYSIS_ASYNC_DONE;
        }
        return;
    }

    if (s_an_state == ANALYSIS_ASYNC_FETCHING_CURVE) {
        if (ok) {
            s_an_result.curve_count = parse_loudness_curve_response(
                s_an_buf.data, s_an_result.curve_db, PLEX_ANALYSIS_CURVE_SAMPLES, s_an_want_tail);
        }
        an_async_cleanup_request();
        s_an_result.valid = true; // gain/loudness/lra came from the metadata stage regardless of curve success
        s_an_state = ANALYSIS_ASYNC_DONE;
        LOG_INFO("Sonic analysis loaded: gain=%.2fdB loudness=%.2f lra=%.2f curve=%d samples (%s)",
                 s_an_result.gain_db, s_an_result.loudness_lufs, s_an_result.lra, s_an_result.curve_count,
                 s_an_want_tail ? "tail" : "head");
    }
}

bool plex_api_analysis_async_is_done(void) {
    return s_an_state == ANALYSIS_ASYNC_DONE;
}

void plex_api_analysis_async_take_result(PlexTrackAnalysis* out) {
    if (!out) return;
    if (s_an_state != ANALYSIS_ASYNC_DONE) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_an_result;
    s_an_state = ANALYSIS_ASYNC_IDLE; // consumed
}

bool plex_api_get_album_art(const char* thumb_path, u8** out_data, size_t* out_size) {
    if (!s_initialized || !thumb_path || !thumb_path[0] || !out_data || !out_size) return false;
    
    char url[1024];
    if (thumb_path[0] == '/') {
        snprintf(url, sizeof(url), "%s%s", s_server_url, thumb_path);
    } else {
        snprintf(url, sizeof(url), "%s/photo/:/transcode?width=128&height=128&minSize=1&url=/library/metadata/%s", s_server_url, thumb_path);
    }
    
    return plex_http_get_binary(url, s_auth_token, out_data, out_size);
}

static int parse_lrc_lyrics(const char* lrc_text, PlexLyricLine* out, int max) {
    if (!lrc_text || !out || max <= 0) return 0;
    
    int count = 0;
    const char* line_start = lrc_text;
    
    while (*line_start && count < max) {
        const char* line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start) : strlen(line_start);
        
        char line_buf[256];
        if (line_len >= sizeof(line_buf)) line_len = sizeof(line_buf) - 1;
        strncpy(line_buf, line_start, line_len);
        line_buf[line_len] = '\0';
        
        int min = 0, sec = 0, ms = 0;
        char lyric_text[128] = "";
        
        if (sscanf(line_buf, "[%d:%d.%d] %[^\r\n]", &min, &sec, &ms, lyric_text) >= 3 ||
            sscanf(line_buf, "[%d:%d] %[^\r\n]", &min, &sec, lyric_text) >= 2) {
            
            if (ms < 100) ms *= 10;
            out[count].time_ms = (min * 60 + sec) * 1000 + ms;
            strncpy(out[count].text, lyric_text, sizeof(out[count].text) - 1);
            out[count].text[sizeof(out[count].text) - 1] = '\0';
            count++;
        }
        
        if (!line_end) break;
        line_start = line_end + 1;
    }
    
    return count;
}

// Parses the body of a GET to a lyric stream's key (e.g. "/library/streams/<id>").
// Not declared in plex_api.h - it's an internal implementation detail of
// plex_api_get_lyrics(), given external linkage only so tests/test_plex_api.c
// can exercise it directly against real captured server responses.
int parse_lyrics_stream_response(const char* response, PlexLyricLine* out, int max) {
    if (!response || !out || max <= 0) return 0;
    int count = 0;

    if (response[0] == '{') {
        // The actual shape Plex returns for a lyrics stream:
        // {"MediaContainer":{"Lyrics":[{"timed":true,"Line":[
        //   {"startOffset":41650,"endOffset":47280,"Span":[{"text":"We're both on holiday", ...}]},
        //   ...
        // ]}]}}
        // Each Line can in principle carry multiple Spans (e.g. word-level
        // timing) - concatenate them to reconstruct the full line text.
        //
        // A Line can also have no Span at all: Plex marks known instrumental
        // breaks this way (a timed entry with nothing to say). Those must be
        // kept, not dropped - they're what let the UI recognize a break and
        // stop highlighting a stale line through it (see
        // find_active_lyric_line() in ui.c). An empty out[].text is exactly
        // that marker; keep any entry with a real timestamp regardless of
        // whether it produced text.
        cJSON* ljson = cJSON_Parse(response);
        if (ljson) {
            cJSON* mc = cJSON_GetObjectItem(ljson, "MediaContainer");
            cJSON* lyrics_arr = mc ? cJSON_GetObjectItem(mc, "Lyrics") : NULL;
            cJSON* lyrics_obj = (lyrics_arr && cJSON_IsArray(lyrics_arr)) ? cJSON_GetArrayItem(lyrics_arr, 0) : NULL;
            cJSON* line_arr = lyrics_obj ? cJSON_GetObjectItem(lyrics_obj, "Line") : NULL;
            if (line_arr && cJSON_IsArray(line_arr)) {
                cJSON* line_item = NULL;
                cJSON_ArrayForEach(line_item, line_arr) {
                    if (count >= max) break;
                    cJSON* start = cJSON_GetObjectItem(line_item, "startOffset");
                    if (!start || !cJSON_IsNumber(start)) continue;

                    cJSON* spans = cJSON_GetObjectItem(line_item, "Span");
                    char line_text[128] = "";
                    if (spans && cJSON_IsArray(spans)) {
                        cJSON* span_item = NULL;
                        cJSON_ArrayForEach(span_item, spans) {
                            cJSON* text = cJSON_GetObjectItem(span_item, "text");
                            if (text && cJSON_IsString(text)) {
                                size_t used = strlen(line_text);
                                if (used < sizeof(line_text) - 1) {
                                    strncat(line_text, text->valuestring, sizeof(line_text) - used - 1);
                                }
                            }
                        }
                    }
                    out[count].time_ms = start->valueint;
                    strncpy(out[count].text, line_text, sizeof(out[count].text) - 1);
                    out[count].text[sizeof(out[count].text) - 1] = '\0';
                    count++;
                }
            }
            cJSON_Delete(ljson);
        }
    } else if (response[0] == '[' && response[1] == '{') {
        // Older/alternate flat-array shape, kept for compatibility:
        // [{"text": "...", "time": 1234}, ...]
        cJSON* ljson = cJSON_Parse(response);
        if (ljson && cJSON_IsArray(ljson)) {
            cJSON* line_item = NULL;
            cJSON_ArrayForEach(line_item, ljson) {
                if (count >= max) break;
                cJSON* text = cJSON_GetObjectItem(line_item, "text");
                cJSON* time = cJSON_GetObjectItem(line_item, "time");
                if (text && cJSON_IsString(text)) {
                    out[count].time_ms = (time && cJSON_IsNumber(time)) ? time->valueint : count * 4000;
                    strncpy(out[count].text, text->valuestring, sizeof(out[count].text) - 1);
                    count++;
                }
            }
            cJSON_Delete(ljson);
        }
    } else {
        // Raw LRC text ("[mm:ss.xx] lyric line" per line), in case a provider
        // ever serves it unwrapped instead of as JSON.
        count = parse_lrc_lyrics(response, out, max);
    }

    return count;
}

int plex_api_get_lyrics(const char* rating_key, PlexLyricLine* out, int max) {
    if (!s_initialized || !rating_key || !rating_key[0] || !out || max <= 0) return 0;
    
    char endpoint[256];
    snprintf(endpoint, sizeof(endpoint), "/library/metadata/%s", rating_key);
    
    char lyric_stream_key[512] = "";
    char lyric_stream_key_fallback[512] = "";
    char* response = NULL;
    
    if (plex_http_get(endpoint, &response)) {
        cJSON* json = cJSON_Parse(response);
        if (json) {
            cJSON* container = cJSON_GetObjectItem(json, "MediaContainer");
            if (container) {
                cJSON* metadata = cJSON_GetObjectItem(container, "Metadata");
                if (metadata && cJSON_IsArray(metadata)) {
                    cJSON* item = cJSON_GetArrayItem(metadata, 0);
                    if (item) {
                        cJSON* media = cJSON_GetObjectItem(item, "Media");
                        if (media && cJSON_IsArray(media)) {
                            cJSON* media_item = cJSON_GetArrayItem(media, 0);
                            if (media_item) {
                                cJSON* part = cJSON_GetObjectItem(media_item, "Part");
                                if (part && cJSON_IsArray(part)) {
                                    cJSON* part_item = cJSON_GetArrayItem(part, 0);
                                    if (part_item) {
                                        cJSON* stream = cJSON_GetObjectItem(part_item, "Stream");
                                        if (stream && cJSON_IsArray(stream)) {
                                            // A track can carry more than one streamType==4 (lyric)
                                            // stream at once - e.g. an "lrc" (time-synced) one and a
                                            // plain "txt" one. Prefer "lrc" explicitly rather than
                                            // whichever happens to come first in the array.
                                            cJSON* s_item = NULL;
                                            cJSON_ArrayForEach(s_item, stream) {
                                                cJSON* streamType = cJSON_GetObjectItem(s_item, "streamType");
                                                cJSON* key = cJSON_GetObjectItem(s_item, "key");
                                                if (streamType && cJSON_IsNumber(streamType) && streamType->valueint == 4 &&
                                                    key && cJSON_IsString(key)) {
                                                    if (lyric_stream_key_fallback[0] == '\0') {
                                                        strncpy(lyric_stream_key_fallback, key->valuestring, sizeof(lyric_stream_key_fallback) - 1);
                                                    }
                                                    cJSON* format = cJSON_GetObjectItem(s_item, "format");
                                                    if (format && cJSON_IsString(format) && strcmp(format->valuestring, "lrc") == 0) {
                                                        strncpy(lyric_stream_key, key->valuestring, sizeof(lyric_stream_key) - 1);
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            cJSON_Delete(json);
        }
        free(response);
    }
    
    if (lyric_stream_key[0] == '\0') {
        strncpy(lyric_stream_key, lyric_stream_key_fallback, sizeof(lyric_stream_key) - 1);
    }
    if (lyric_stream_key[0] == '\0') {
        snprintf(lyric_stream_key, sizeof(lyric_stream_key), "/library/metadata/%s/subtitles", rating_key);
    }
    
    char* lrc_response = NULL;
    int count = 0;
    if (plex_http_get(lyric_stream_key, &lrc_response)) {
        count = parse_lyrics_stream_response(lrc_response, out, max);
        free(lrc_response);
    }

    if (count == 0) {
        out[0].time_ms = 0;
        strncpy(out[0].text, "No time-synced lyrics available", sizeof(out[0].text) - 1);
        count = 1;
    }
    
    LOG_INFO("Loaded %d lyric lines for track %s", count, rating_key);
    return count;
}
