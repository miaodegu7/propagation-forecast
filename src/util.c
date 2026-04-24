#include "app.h"
#include <locale.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef _WIN32
static int utf16_to_utf8(const wchar_t *src, char *dst, size_t dst_len) {
    if (!src || !dst || dst_len == 0) {
        return -1;
    }
    int written = WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, (int)dst_len, NULL, NULL);
    if (written <= 0) {
        if (dst_len > 0) {
            dst[0] = '\0';
        }
        return -1;
    }
    return 0;
}

int app_windows_path_to_utf16(const char *path, wchar_t *out, size_t out_len) {
    if (!path || !out || out_len == 0) {
        return -1;
    }
    int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, (int)out_len);
    if (written > 0) {
        return 0;
    }
    written = MultiByteToWideChar(CP_ACP, 0, path, -1, out, (int)out_len);
    if (written > 0) {
        return 0;
    }
    out[0] = L'\0';
    return -1;
}

static int get_exe_path_utf16(wchar_t *out, size_t out_len) {
    if (!out || out_len == 0) {
        return -1;
    }
    DWORD len = GetModuleFileNameW(NULL, out, (DWORD)out_len);
    if (len == 0 || len >= out_len) {
        out[0] = L'\0';
        return -1;
    }
    return 0;
}

static void set_cwd_to_exe_dir(void) {
    wchar_t path[1024];
    if (get_exe_path_utf16(path, sizeof(path) / sizeof(path[0])) != 0) {
        return;
    }
    wchar_t *slash = wcsrchr(path, L'\\');
    if (!slash) {
        return;
    }
    *slash = L'\0';
    SetCurrentDirectoryW(path);
}

static void append_line_to_boot_log(const char *line) {
    FILE *fp = _wfopen(L"propagation_bot.log", L"ab+");
    if (!fp) {
        return;
    }
    if (fseek(fp, 0, SEEK_END) == 0 && ftell(fp) == 0) {
        static const unsigned char utf8_bom[] = {0xEF, 0xBB, 0xBF};
        fwrite(utf8_bom, 1, sizeof(utf8_bom), fp);
    }
    fwrite(line, 1, strlen(line), fp);
    fclose(fp);
}

static const char *windows_tz_env_value(const char *tz_name) {
    if (!tz_name || !*tz_name) {
        return NULL;
    }
    struct {
        const char *name;
        const char *value;
    } aliases[] = {
        {"Asia/Shanghai", "CST-8"},
        {"Asia/Chongqing", "CST-8"},
        {"Asia/Hong_Kong", "HKT-8"},
        {"Asia/Singapore", "SGT-8"},
        {"Asia/Tokyo", "JST-9"},
        {"UTC", "UTC0"},
        {"Etc/UTC", "UTC0"},
        {"Etc/GMT", "GMT0"},
        {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"},
        {"America/Denver", "MST7MDT,M3.2.0/2,M11.1.0/2"},
        {"America/Chicago", "CST6CDT,M3.2.0/2,M11.1.0/2"},
        {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
        {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
        {"Europe/Berlin", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
        {"Europe/Paris", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (strcmp(tz_name, aliases[i].name) == 0) {
            return aliases[i].value;
        }
    }
    return NULL;
}
#endif

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

int app_render_template(char *out, size_t out_len, const char *tmpl,
                        const template_token_t *tokens, size_t token_count) {
    sb_t sb;
    const char *p = tmpl ? tmpl : "";

    sb_init(&sb);
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                char name[128];
                size_t len = (size_t)(end - (p + 2));
                const char *value = "";
                if (len >= sizeof(name)) {
                    len = sizeof(name) - 1;
                }
                memcpy(name, p + 2, len);
                name[len] = '\0';
                trim_whitespace(name);
                for (size_t i = 0; i < token_count; ++i) {
                    if (strcmp(tokens[i].name, name) == 0) {
                        value = tokens[i].value ? tokens[i].value : "";
                        break;
                    }
                }
                sb_append(&sb, value);
                p = end + 2;
                continue;
            }
        }
        char ch[2] = {*p++, '\0'};
        sb_append(&sb, ch);
    }

    copy_string(out, out_len, sb.data ? sb.data : "");
    sb_free(&sb);
    return 0;
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

int csv_contains_ci(const char *csv, const char *needle) {
    if (!csv || !*csv || !needle || !*needle) {
        return 0;
    }
    char temp[MAX_HUGE_TEXT];
    copy_string(temp, sizeof(temp), csv);
    char *save = NULL;
    for (char *part = strtok_r(temp, ",|/ \t\r\n", &save); part; part = strtok_r(NULL, ",|/ \t\r\n", &save)) {
        trim_whitespace(part);
        if (*part && strcasecmp(part, needle) == 0) {
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

char *base64_encode_alloc(const unsigned char *data, size_t len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    size_t in_pos = 0;
    size_t out_pos = 0;
    if (!out) {
        return NULL;
    }

    while (in_pos < len) {
        size_t remaining = len - in_pos;
        unsigned int octet_a = data[in_pos++];
        unsigned int octet_b = remaining > 1 ? data[in_pos++] : 0;
        unsigned int octet_c = remaining > 2 ? data[in_pos++] : 0;
        unsigned int triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        out[out_pos++] = alphabet[(triple >> 18) & 0x3Fu];
        out[out_pos++] = alphabet[(triple >> 12) & 0x3Fu];
        out[out_pos++] = remaining > 1 ? alphabet[(triple >> 6) & 0x3Fu] : '=';
        out[out_pos++] = remaining > 2 ? alphabet[triple & 0x3Fu] : '=';
    }
    out[out_len] = '\0';
    return out;
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

#ifdef _WIN32
static void configure_curl_ssl(CURL *curl) {
#ifdef CURLSSLOPT_NATIVE_CA
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#else
    (void)curl;
#endif
}
#else
static void configure_curl_ssl(CURL *curl) {
    (void)curl;
}
#endif

static char *http_request_common(const char *url, const char *bearer_token, const char *json_body, long *status_code, size_t *out_size) {
    curl_global_once();
    CURL *curl = curl_easy_init();
    if (!curl) {
        return NULL;
    }

    memory_block_t mem = {0};
    struct curl_slist *headers = NULL;
    char error_buffer[CURL_ERROR_SIZE];
    memset(error_buffer, 0, sizeof(error_buffer));
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
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    configure_curl_ssl(curl);
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
    if (out_size) {
        *out_size = mem.size;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        app_write_boot_log("HTTP 请求失败: %s (%s)",
            url ? url : "(null)",
            error_buffer[0] ? error_buffer : curl_easy_strerror(rc));
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
    return http_request_common(url, bearer_token, NULL, status_code, NULL);
}

char *http_get_binary(const char *url, const char *bearer_token, long *status_code, size_t *out_size) {
    return http_request_common(url, bearer_token, NULL, status_code, out_size);
}

char *http_post_json(const char *url, const char *bearer_token, const char *json_body, long *status_code) {
    return http_request_common(url, bearer_token, json_body, status_code, NULL);
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
#ifdef _WIN32
    const char *env_value = windows_tz_env_value(tz_name);
    if (!env_value) {
        if (strchr(tz_name, '/')) {
            return;
        }
        env_value = tz_name;
    }
    _putenv_s("TZ", env_value);
    _tzset();
#else
    setenv("TZ", tz_name, 1);
    tzset();
#endif
}

void app_sleep_ms(int milliseconds) {
    if (milliseconds <= 0) {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

void app_default_db_path(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    copy_string(out, out_len, "./propagation.db");
}

void app_prepare_desktop_mode(int hide_console) {
#ifdef _WIN32
    set_cwd_to_exe_dir();
    setlocale(LC_ALL, ".UTF-8");
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if (hide_console) {
        HWND console = GetConsoleWindow();
        if (console) {
            ShowWindow(console, SW_HIDE);
        }
    }
#else
    (void)hide_console;
#endif
}

void app_write_boot_log(const char *fmt, ...) {
    char ts[64];
    format_time_local(time(NULL), ts, sizeof(ts));

    char message[MAX_LOG_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    char line[MAX_LOG_TEXT + 96];
    snprintf(line, sizeof(line), "[%s] %s\n", ts, message);

#ifdef _WIN32
    append_line_to_boot_log(line);
#else
    FILE *fp = fopen("propagation_bot.log", "ab");
    if (fp) {
        fwrite(line, 1, strlen(line), fp);
        fclose(fp);
    }
#endif
}

void app_set_last_error(app_t *app, const char *fmt, ...) {
    char message[MAX_LOG_TEXT];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (app) {
        copy_string(app->last_error, sizeof(app->last_error), message);
    }
    app_write_boot_log("%s", message);
}

void app_show_startup_error(const char *title, const char *message) {
#ifdef _WIN32
    wchar_t wide_title[256];
    wchar_t wide_message[2048];
    if (app_windows_path_to_utf16(title ? title : "程序启动失败", wide_title, sizeof(wide_title) / sizeof(wide_title[0])) != 0) {
        wcscpy(wide_title, L"程序启动失败");
    }
    if (app_windows_path_to_utf16(message ? message : "请查看 propagation_bot.log", wide_message, sizeof(wide_message) / sizeof(wide_message[0])) != 0) {
        wcscpy(wide_message, L"请查看 propagation_bot.log");
    }
    MessageBoxW(NULL, wide_message, wide_title, MB_ICONERROR | MB_OK);
#else
    (void)title;
    (void)message;
#endif
}

int app_create_thread(pthread_t *thread, void *(*start_routine)(void *), void *arg, size_t stack_size) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        return pthread_create(thread, NULL, start_routine, arg);
    }
    if (stack_size > 0) {
        (void)pthread_attr_setstacksize(&attr, stack_size);
    }
    rc = pthread_create(thread, &attr, start_routine, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

void app_open_admin_console(const settings_t *settings) {
#ifdef _WIN32
    if (!settings) {
        return;
    }
    char host[MAX_TEXT];
    const char *bind_addr = settings->bind_addr[0] ? settings->bind_addr : "127.0.0.1";
    if (strcmp(bind_addr, "0.0.0.0") == 0 || strcmp(bind_addr, "::") == 0 || strcmp(bind_addr, "*") == 0) {
        bind_addr = "127.0.0.1";
    }
    copy_string(host, sizeof(host), bind_addr);

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/", host, settings->http_port);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    (void)settings;
#endif
}

int app_net_init(void) {
#ifdef _WIN32
    WSADATA wsa_data;
    return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

void app_net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
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
    app_write_boot_log("%s: %s", level ? level : "INFO", message);

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

static int write_utf8_text_file(const char *path, const char *text) {
#ifdef _WIN32
    wchar_t wide_path[1024];
    FILE *fp = NULL;
    if (app_windows_path_to_utf16(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])) != 0) {
        return -1;
    }
    fp = _wfopen(wide_path, L"wb");
#else
    FILE *fp = fopen(path, "wb");
#endif
    if (!fp) {
        return -1;
    }
    if (text && *text) {
        fwrite(text, 1, strlen(text), fp);
    }
    fclose(fp);
    return 0;
}

static int read_binary_file_alloc(const char *path, unsigned char **out_data, size_t *out_size) {
    long size = 0;
    size_t read_size = 0;
    unsigned char *buffer = NULL;
    FILE *fp = NULL;

    if (!out_data || !out_size) {
        return -1;
    }
    *out_data = NULL;
    *out_size = 0;

#ifdef _WIN32
    wchar_t wide_path[1024];
    if (app_windows_path_to_utf16(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])) != 0) {
        return -1;
    }
    fp = _wfopen(wide_path, L"rb");
#else
    fp = fopen(path, "rb");
#endif
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    size = ftell(fp);
    if (size < 0) {
        fclose(fp);
        return -1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    buffer = malloc((size_t)size);
    if (!buffer) {
        fclose(fp);
        return -1;
    }
    read_size = fread(buffer, 1, (size_t)size, fp);
    fclose(fp);
    if (read_size != (size_t)size) {
        free(buffer);
        return -1;
    }

    *out_data = buffer;
    *out_size = read_size;
    return 0;
}

static void delete_file_if_exists(const char *path) {
    if (!path || !*path) {
        return;
    }
#ifdef _WIN32
    wchar_t wide_path[1024];
    if (app_windows_path_to_utf16(path, wide_path, sizeof(wide_path) / sizeof(wide_path[0])) == 0) {
        _wremove(wide_path);
    }
#else
    unlink(path);
#endif
}

static int create_temp_html_png_paths(const char *stem, char *html_path, size_t html_len, char *png_path, size_t png_len) {
    const char *prefix = (stem && *stem) ? stem : "propagation";
#ifdef _WIN32
    wchar_t temp_dir[MAX_PATH];
    wchar_t temp_file[MAX_PATH];
    char temp_utf8[1024];
    char *dot = NULL;

    if (!GetTempPathW((DWORD)(sizeof(temp_dir) / sizeof(temp_dir[0])), temp_dir)) {
        return -1;
    }
    if (!GetTempFileNameW(temp_dir, L"psk", 0, temp_file)) {
        return -1;
    }
    DeleteFileW(temp_file);
    if (utf16_to_utf8(temp_file, temp_utf8, sizeof(temp_utf8)) != 0) {
        return -1;
    }
    dot = strrchr(temp_utf8, '.');
    if (dot) {
        *dot = '\0';
    }
    snprintf(html_path, html_len, "%s-%s.html", temp_utf8, prefix);
    snprintf(png_path, png_len, "%s-%s.png", temp_utf8, prefix);
    return 0;
#else
    char base_template[] = "/tmp/pskXXXXXX";
    int fd = mkstemp(base_template);
    if (fd < 0) {
        return -1;
    }
    close(fd);
    unlink(base_template);
    snprintf(html_path, html_len, "%s-%s.html", base_template, prefix);
    snprintf(png_path, png_len, "%s-%s.png", base_template, prefix);
    return 0;
#endif
}

static int path_to_file_url(const char *path, char *out, size_t out_len) {
    size_t pos = 0;
    if (!path || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';

#ifdef _WIN32
    pos += (size_t)snprintf(out + pos, out_len - pos, "file:///");
#else
    pos += (size_t)snprintf(out + pos, out_len - pos, "file://");
#endif

    for (const unsigned char *p = (const unsigned char *)path; *p && pos + 4 < out_len; ++p) {
        unsigned char ch = *p == '\\' ? '/' : *p;
        if (isalnum(ch) || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~' || ch == ':') {
            out[pos++] = (char)ch;
        } else {
            snprintf(out + pos, out_len - pos, "%%%02X", ch);
            pos += 3;
        }
    }
    out[pos] = '\0';
    return 0;
}

#ifdef _WIN32
static int find_headless_browser_path(char *out, size_t out_len) {
    char base[MAX_PATH];
    char candidate[MAX_PATH * 2];
    const char *tails[] = {
        "Microsoft\\Edge\\Application\\msedge.exe",
        "Google\\Chrome\\Application\\chrome.exe",
        "Chromium\\Application\\chrome.exe"
    };
    const char *envs[] = {"ProgramFiles(x86)", "ProgramFiles", "LocalAppData"};

    for (size_t e = 0; e < sizeof(envs) / sizeof(envs[0]); ++e) {
        DWORD got = GetEnvironmentVariableA(envs[e], base, (DWORD)sizeof(base));
        if (got == 0 || got >= sizeof(base)) {
            continue;
        }
        for (size_t i = 0; i < sizeof(tails) / sizeof(tails[0]); ++i) {
            snprintf(candidate, sizeof(candidate), "%s\\%s", base, tails[i]);
            if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
                copy_string(out, out_len, candidate);
                return 0;
            }
        }
    }
    return -1;
}

static int run_browser_capture(const char *html_path, const char *png_path, char *detail, size_t detail_len) {
    char browser_path[MAX_PATH * 2];
    char file_url[2048];
    wchar_t browser_w[1024];
    wchar_t screenshot_arg[1400];
    wchar_t url_w[2048];

    if (detail && detail_len > 0) {
        detail[0] = '\0';
    }
    if (find_headless_browser_path(browser_path, sizeof(browser_path)) != 0) {
        copy_string(detail, detail_len, "未找到 Edge/Chrome，无法生成快照图");
        return -1;
    }
    if (path_to_file_url(html_path, file_url, sizeof(file_url)) != 0) {
        copy_string(detail, detail_len, "本地快照 URL 构建失败");
        return -1;
    }
    if (app_windows_path_to_utf16(browser_path, browser_w, sizeof(browser_w) / sizeof(browser_w[0])) != 0 ||
        app_windows_path_to_utf16(png_path, screenshot_arg, sizeof(screenshot_arg) / sizeof(screenshot_arg[0])) != 0 ||
        app_windows_path_to_utf16(file_url, url_w, sizeof(url_w) / sizeof(url_w[0])) != 0) {
        copy_string(detail, detail_len, "浏览器截图参数转换失败");
        return -1;
    }

    {
        wchar_t screenshot_opt[1400];
        intptr_t rc = 0;
        _snwprintf(screenshot_opt, sizeof(screenshot_opt) / sizeof(screenshot_opt[0]),
            L"--screenshot=%ls", screenshot_arg);
        rc = _wspawnl(_P_WAIT, browser_w, browser_w,
            L"--headless=new",
            L"--disable-gpu",
            L"--hide-scrollbars",
            L"--window-size=1400,980",
            screenshot_opt,
            url_w,
            NULL);
        if (rc != 0) {
            snprintf(detail, detail_len, "浏览器截图失败，退出码=%lld", (long long)rc);
            return -1;
        }
    }
    return 0;
}
#else
static const char *linux_browser_candidates[] = {
    "microsoft-edge",
    "microsoft-edge-stable",
    "google-chrome",
    "chromium-browser",
    "chromium",
    "msedge"
};

static int run_browser_capture(const char *html_path, const char *png_path, char *detail, size_t detail_len) {
    char file_url[2048];
    char screenshot_opt[1200];
    int status = -1;

    if (detail && detail_len > 0) {
        detail[0] = '\0';
    }
    if (path_to_file_url(html_path, file_url, sizeof(file_url)) != 0) {
        copy_string(detail, detail_len, "本地快照 URL 构建失败");
        return -1;
    }
    snprintf(screenshot_opt, sizeof(screenshot_opt), "--screenshot=%s", png_path);

    for (size_t i = 0; i < sizeof(linux_browser_candidates) / sizeof(linux_browser_candidates[0]); ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp(linux_browser_candidates[i], linux_browser_candidates[i],
                "--headless=new",
                "--disable-gpu",
                "--hide-scrollbars",
                "--window-size=1400,980",
                screenshot_opt,
                file_url,
                (char *)NULL);
            execlp(linux_browser_candidates[i], linux_browser_candidates[i],
                "--headless",
                "--disable-gpu",
                "--hide-scrollbars",
                "--window-size=1400,980",
                screenshot_opt,
                file_url,
                (char *)NULL);
            _exit(127);
        }
        if (pid < 0) {
            continue;
        }
        if (waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            return 0;
        }
    }

    copy_string(detail, detail_len, "未找到可用的无头浏览器，无法生成快照图");
    return -1;
}
#endif

int app_capture_html_to_png(const char *html_utf8, const char *stem, unsigned char **png_data, size_t *png_size, char *detail, size_t detail_len) {
    char html_path[1024];
    char png_path[1024];
    int rc = -1;

    if (png_data) {
        *png_data = NULL;
    }
    if (png_size) {
        *png_size = 0;
    }
    if (detail && detail_len > 0) {
        detail[0] = '\0';
    }
    if (!html_utf8 || !png_data || !png_size) {
        copy_string(detail, detail_len, "截图参数不完整");
        return -1;
    }
    if (create_temp_html_png_paths(stem, html_path, sizeof(html_path), png_path, sizeof(png_path)) != 0) {
        copy_string(detail, detail_len, "临时文件路径创建失败");
        return -1;
    }
    if (write_utf8_text_file(html_path, html_utf8) != 0) {
        copy_string(detail, detail_len, "快照 HTML 写入失败");
        delete_file_if_exists(html_path);
        delete_file_if_exists(png_path);
        return -1;
    }
    if (run_browser_capture(html_path, png_path, detail, detail_len) != 0) {
        delete_file_if_exists(html_path);
        delete_file_if_exists(png_path);
        return -1;
    }
    rc = read_binary_file_alloc(png_path, png_data, png_size);
    if (rc != 0) {
        copy_string(detail, detail_len, "截图输出文件读取失败");
    }
    delete_file_if_exists(html_path);
    delete_file_if_exists(png_path);
    return rc == 0 ? 0 : -1;
}
