#ifndef PROPAGATION_APP_H
#define PROPAGATION_APP_H

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <math.h>
#include <mosquitto.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define APP_NAME "Propagation Forecast Bot"
#define APP_VERSION "0.1.0"

#define MAX_SMALL_TEXT 64
#define MAX_TEXT 128
#define MAX_LARGE_TEXT 256
#define MAX_REPORT_TEXT 8192
#define MAX_LOG_TEXT 512
#define MAX_SPOTS 1024
#define MAX_TARGETS 128
#define MAX_BANDS 16
#define MAX_VHF 16

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sb_t;

typedef enum {
    TARGET_GROUP = 1,
    TARGET_PRIVATE = 2
} target_type_t;

typedef struct {
    int id;
    target_type_t type;
    int enabled;
    int command_enabled;
    char label[MAX_TEXT];
    char target_id[MAX_TEXT];
    char notes[MAX_LARGE_TEXT];
} target_t;

typedef struct {
    char bind_addr[MAX_TEXT];
    int http_port;
    char admin_user[MAX_TEXT];
    char admin_password[MAX_TEXT];
    char station_name[MAX_TEXT];
    char station_grid[MAX_TEXT];
    double latitude;
    double longitude;
    char timezone[MAX_TEXT];
    char onebot_api_base[MAX_LARGE_TEXT];
    char onebot_access_token[MAX_LARGE_TEXT];
    char onebot_webhook_token[MAX_LARGE_TEXT];
    char schedule_morning[MAX_SMALL_TEXT];
    char schedule_evening[MAX_SMALL_TEXT];
    int morning_enabled;
    int evening_enabled;
    int refresh_interval_minutes;
    int psk_radius_km;
    int psk_window_minutes;
} settings_t;

typedef struct {
    char name[MAX_TEXT];
    char time_slot[MAX_SMALL_TEXT];
    char status[MAX_TEXT];
} band_condition_t;

typedef struct {
    char name[MAX_TEXT];
    char location[MAX_TEXT];
    char status[MAX_TEXT];
} vhf_condition_t;

typedef struct {
    int valid;
    char updated[MAX_TEXT];
    char source_url[MAX_LARGE_TEXT];
    int solarflux;
    int aindex;
    int kindex;
    char kindex_text[MAX_TEXT];
    char xray[MAX_TEXT];
    int sunspots;
    double heliumline;
    int protonflux;
    int electronflux;
    int aurora;
    double normalization;
    double latdegree;
    double solarwind;
    double magneticfield;
    char geomagfield[MAX_TEXT];
    char signalnoise[MAX_TEXT];
    char muf[MAX_TEXT];
    band_condition_t bands[MAX_BANDS];
    int band_count;
    vhf_condition_t vhf[MAX_VHF];
    int vhf_count;
} hamqsl_data_t;

typedef struct {
    int valid;
    char current_time[MAX_TEXT];
    int is_day;
    double temperature_c;
    int humidity;
    double dewpoint_c;
    double pressure_hpa;
    int cloud_cover;
    double visibility_m;
    double wind_kmh;
    double cape;
    double lifted_index;
    int precipitation_probability;
    char sunrise[MAX_TEXT];
    char sunset[MAX_TEXT];
    double daylight_hours;
    double tmax_c;
    double tmin_c;
    int daily_precip_probability;
} weather_data_t;

typedef struct {
    int in_use;
    time_t timestamp;
    double frequency_hz;
    int snr;
    int sender_adif;
    int receiver_adif;
    char band[MAX_SMALL_TEXT];
    char mode[MAX_SMALL_TEXT];
    char sender_call[MAX_TEXT];
    char sender_grid[MAX_TEXT];
    char receiver_call[MAX_TEXT];
    char receiver_grid[MAX_TEXT];
} psk_spot_t;

typedef struct {
    int mqtt_connected;
    int global_spots_15m;
    int global_spots_60m;
    int local_spots_15m;
    int local_spots_60m;
    int longest_path_km;
    int best_snr;
    char latest_local_time[MAX_TEXT];
    char latest_pair[MAX_LARGE_TEXT];
    char farthest_peer[MAX_TEXT];
    char farthest_grid[MAX_TEXT];
    char assessment[MAX_TEXT];
    char confidence[MAX_TEXT];
    int score;
} psk_summary_t;

typedef struct {
    time_t refreshed_at;
    hamqsl_data_t hamqsl;
    weather_data_t weather;
    psk_summary_t psk;
    char report_text[MAX_REPORT_TEXT];
    char report_6m[MAX_REPORT_TEXT];
    char report_solar[MAX_REPORT_TEXT];
    char analysis_summary[1024];
    char sun_summary[512];
} snapshot_t;

typedef struct {
    sqlite3 *db;
    pthread_mutex_t db_mutex;
    pthread_mutex_t cache_mutex;
    pthread_mutex_t refresh_mutex;
    pthread_mutex_t spot_mutex;
    settings_t settings;
    snapshot_t snapshot;
    psk_spot_t spots[MAX_SPOTS];
    size_t spot_head;
    size_t spot_count;
    struct mosquitto *mosq;
    int mqtt_connected;
    int running;
    int http_fd;
} app_t;

typedef struct {
    char *data;
    size_t size;
} memory_block_t;

typedef struct {
    char method[8];
    char path[256];
    char query[512];
    char authorization[512];
    char content_type[128];
    char body[16384];
    size_t body_len;
} http_request_t;

void sb_init(sb_t *sb);
void sb_free(sb_t *sb);
int sb_append(sb_t *sb, const char *text);
int sb_appendf(sb_t *sb, const char *fmt, ...);

void trim_whitespace(char *text);
void copy_string(char *dst, size_t dst_len, const char *src);
void format_time_local(time_t when, char *out, size_t out_len);
void format_iso_date_local(time_t when, char *out, size_t out_len);
void html_escape_to_sb(sb_t *sb, const char *text);
void url_decode_inplace(char *text);
int parse_hhmm(const char *text);
int string_contains_ci(const char *haystack, const char *needle);
double clamp_double(double value, double min_value, double max_value);
int clamp_int(int value, int min_value, int max_value);
int base64_decode(const char *input, unsigned char *output, size_t *output_len);
void app_log(app_t *app, const char *level, const char *fmt, ...);
char *http_get_text(const char *url, const char *bearer_token, long *status_code);
char *http_post_json(const char *url, const char *bearer_token, const char *json_body, long *status_code);
int grid_to_latlon(const char *grid, double *lat, double *lon);
double haversine_km(double lat1, double lon1, double lat2, double lon2);
int prefix_matches_grid(const char *grid, const char *prefix);
void apply_timezone(const char *tz_name);

int storage_init(app_t *app, const char *db_path);
int storage_load_settings(app_t *app, settings_t *out);
int storage_save_setting(app_t *app, const char *key, const char *value);
int storage_load_targets(app_t *app, target_t *targets, int max_targets, int *out_count);
int storage_add_target(app_t *app, const target_t *target);
int storage_delete_target(app_t *app, int target_id);
int storage_toggle_target(app_t *app, int target_id, int enabled);
int storage_set_last_fire(app_t *app, const char *slot, const char *date_text);
int storage_get_last_fire(app_t *app, const char *slot, char *out, size_t out_len);
int storage_load_recent_logs(app_t *app, sb_t *html_rows);

int fetch_hamqsl_data(hamqsl_data_t *out);
int fetch_weather_data(const settings_t *settings, weather_data_t *out);
int refresh_snapshot(app_t *app, int force);
void build_reports(app_t *app, snapshot_t *snapshot);
int onebot_send_message(app_t *app, target_type_t type, const char *target_id, const char *message);
int send_report_to_all_targets(app_t *app, const char *message);

int psk_start(app_t *app);
void psk_stop(app_t *app);
void psk_compute_summary(app_t *app, const settings_t *settings, psk_summary_t *out);
void psk_append_recent_rows(app_t *app, sb_t *rows, const settings_t *settings, int max_rows);

int http_server_run(app_t *app);

#endif
