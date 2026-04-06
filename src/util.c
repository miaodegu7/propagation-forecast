#include "app.h"

static size_t curl_write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    memory_block_t *mem = (memory_block_t *)userp;
    char *next = realloc(mem->data, mem->size + realsize + 1);
    if (next == NULL) {
        return 0;
    }
    mem->data = next;
    memcpy(mem->data + mem->size, contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

void sb_init(sb_t *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(sb_t *sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

static int sb_reserve(sb_t *sb, size_t extra) {
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap) {
        return 0;
    }
    size_t next_cap = sb->cap ? sb->cap : 256;
    while (next_cap < need) {
        next_cap *= 2;
    }
    char *next = realloc(sb->data, next_cap);
    if (!next) {
        return -1;
    }
    sb->data = next;
    sb->cap = next_cap;
    if (sb->len == 0) {
        sb->data[0] = '\0';
    }
    return 0;
}

int sb_append(sb_t *sb, const char *text) {
    size_t add = strlen(text);
    if (sb_reserve(sb, add) != 0) {
        return -1;
    }
    memcpy(sb->data + sb->len, text, add);
    sb->len += add;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_appendf(sb_t *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return -1;
    }
    if (sb_reserve(sb, (size_t)needed) != 0) {
        va_end(args);
        return -1;
    }
    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, args);
    sb->len += (size_t)needed;
    va_end(args);
    return 0;
}

void trim_whitespace(char *text) {
    if (!text || !*text) {
        return;
    }
    char *start = text;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[len - 1] = '\0';
        len--;
    }
}

void copy_string(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_len, "%s", src);
}

void format_time_local(time_t when, char *out, size_t out_len) {
    struct tm tm_local;
    localtime_r(&when, &tm_local);
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tm_local);
}

void format_iso_date_local(time_t when, char *out, size_t out_len) {
    struct tm tm_local;
    localtime_r(&when, &tm_local);
    strftime(out, out_len, "%Y-%m-%d", &tm_local);
}

void html_escape_to_sb(sb_t *sb, const char *text) {
    const char *p = text ? text : "";
    while (*p) {
        switch (*p) {
            case '&':
                sb_append(sb, "&amp;");
                break;
            case '<':
                sb_append(sb, "&lt;");
                break;
            case '>':
                sb_append(sb, "&gt;");
                break;
            case '"':
                sb_append(sb, "&quot;");
                break;
            case '\'':
                sb_append(sb, "&#39;");
                break;
            default: {
                char temp[2] = {*p, '\0'};
                sb_append(sb, temp);
                break;
            }
        }
        p++;
    }
}

void url_decode_inplace(char *text) {
    char *src = text;
    char *dst = text;
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

int parse_hhmm(const char *text) {
    if (!text || strlen(text) != 5 || text[2] != ':') {
        return -1;
    }
    int hh = (text[0] - '0') * 10 + (text[1] - '0');
    int mm = (text[3] - '0') * 10 + (text[4] - '0');
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return -1;
    }
    return hh * 60 + mm;
}

int string_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) {
        return 0;
    }
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        if (strncasecmp(p, needle, nlen) == 0) {
            return 1;
        }
    }
    return 0;
}

double clamp_double(double value, double min_value, double max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

int clamp_int(int value, int min_value, int max_value) {
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

int base64_decode(const char *input, unsigned char *output, size_t *output_len) {
    unsigned int buffer = 0;
    int bits = 0;
    size_t out_pos = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p; ++p) {
        if (*p == '=') {
            break;
        }
        if (isspace(*p)) {
            continue;
        }
        signed char value = -1;
        if (*p >= 'A' && *p <= 'Z') value = (signed char)(*p - 'A');
        else if (*p >= 'a' && *p <= 'z') value = (signed char)(*p - 'a' + 26);
        else if (*p >= '0' && *p <= '9') value = (signed char)(*p - '0' + 52);
        else if (*p == '+') value = 62;
        else if (*p == '/') value = 63;
        if (value < 0) {
            return -1;
        }
        buffer = (buffer << 6) | (unsigned int)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output[out_pos++] = (unsigned char)((buffer >> bits) & 0xFFu);
        }
    }
    if (output_len) {
        *output_len = out_pos;
    }
    return 0;
}

static void curl_global_once(void) {
    static pthread_mutex_t once_mutex = PTHREAD_MUTEX_INITIALIZER;
    static int initialized = 0;
    pthread_mutex_lock(&once_mutex);
    if (!initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        initialized = 1;
    }
    pthread_mutex_unlock(&once_mutex);
}

static char *http_request_common(const char *url, const char *bearer_token, const char *json_body, long *status_code) {
    curl_global_once();
    CURL *curl = curl_easy_init();
    if (!curl) {
        return NULL;
    }

    memory_block_t mem = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: PropagationForecastBot/0.1");
    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    if (json_body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }
    if (bearer_token && *bearer_token) {
        char auth[512];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer_token);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (json_body) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    }

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (status_code) {
        *status_code = code;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(mem.data);
        return NULL;
    }

    if (!mem.data) {
        mem.data = malloc(1);
        if (mem.data) {
            mem.data[0] = '\0';
        }
    }
    return mem.data;
}

char *http_get_text(const char *url, const char *bearer_token, long *status_code) {
    return http_request_common(url, bearer_token, NULL, status_code);
}

char *http_post_json(const char *url, const char *bearer_token, const char *json_body, long *status_code) {
    return http_request_common(url, bearer_token, json_body, status_code);
}

int grid_to_latlon(const char *grid, double *lat, double *lon) {
    if (!grid) {
        return -1;
    }
    size_t len = strlen(grid);
    if (len < 4) {
        return -1;
    }
    char a = (char)toupper((unsigned char)grid[0]);
    char b = (char)toupper((unsigned char)grid[1]);
    char c = grid[2];
    char d = grid[3];
    if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || !isdigit((unsigned char)c) || !isdigit((unsigned char)d)) {
        return -1;
    }

    double lon_value = -180.0 + (double)(a - 'A') * 20.0 + (double)(c - '0') * 2.0;
    double lat_value = -90.0 + (double)(b - 'A') * 10.0 + (double)(d - '0');
    double lon_step = 2.0;
    double lat_step = 1.0;

    if (len >= 6) {
        char e = (char)tolower((unsigned char)grid[4]);
        char f = (char)tolower((unsigned char)grid[5]);
        if (e < 'a' || e > 'x' || f < 'a' || f > 'x') {
            return -1;
        }
        lon_value += (double)(e - 'a') * (5.0 / 60.0);
        lat_value += (double)(f - 'a') * (2.5 / 60.0);
        lon_step = 5.0 / 60.0;
        lat_step = 2.5 / 60.0;
    }

    if (len >= 8) {
        char g = grid[6];
        char h = grid[7];
        if (!isdigit((unsigned char)g) || !isdigit((unsigned char)h)) {
            return -1;
        }
        lon_value += (double)(g - '0') * (0.5 / 60.0);
        lat_value += (double)(h - '0') * (0.25 / 60.0);
        lon_step = 0.5 / 60.0;
        lat_step = 0.25 / 60.0;
    }

    if (lon) {
        *lon = lon_value + lon_step / 2.0;
    }
    if (lat) {
        *lat = lat_value + lat_step / 2.0;
    }
    return 0;
}

double haversine_km(double lat1, double lon1, double lat2, double lon2) {
    const double earth_radius_km = 6371.0;
    double dlat = (lat2 - lat1) * (M_PI / 180.0);
    double dlon = (lon2 - lon1) * (M_PI / 180.0);
    double a = sin(dlat / 2.0) * sin(dlat / 2.0)
        + cos(lat1 * (M_PI / 180.0)) * cos(lat2 * (M_PI / 180.0))
        * sin(dlon / 2.0) * sin(dlon / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return earth_radius_km * c;
}

int prefix_matches_grid(const char *grid, const char *prefix) {
    if (!grid || !prefix || !*grid || !*prefix) {
        return 0;
    }
    size_t prefix_len = strlen(prefix);
    if (strlen(grid) < prefix_len) {
        return 0;
    }
    return strncasecmp(grid, prefix, prefix_len) == 0;
}

void apply_timezone(const char *tz_name) {
    if (!tz_name || !*tz_name) {
        return;
    }
    setenv("TZ", tz_name, 1);
    tzset();
}

void app_log(app_t *app, const char *level, const char *fmt, ...) {
    char message[MAX_LOG_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    char ts[64];
    format_time_local(time(NULL), ts, sizeof(ts));
    fprintf(stderr, "[%s] %s: %s\n", ts, level ? level : "INFO", message);

    if (!app || !app->db) {
        return;
    }

    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO logs(created_at, level, message) VALUES(?, ?, ?)";
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ts, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, level ? level : "INFO", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, message, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
}
