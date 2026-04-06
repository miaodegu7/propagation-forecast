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
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <io.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#define localtime_r(timer, result) (localtime_s((result), (timer)) == 0 ? (result) : NULL)
#define sleep(seconds) Sleep((DWORD)((seconds) * 1000))
#define close closesocket
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef _WIN32
typedef SOCKET app_socket_t;
#define APP_INVALID_SOCKET INVALID_SOCKET
#else
typedef int app_socket_t;
#define APP_INVALID_SOCKET (-1)
#endif

#define APP_NAME "业余无线电传播助手"
#define APP_VERSION "0.2.0"

#define MAX_SMALL_TEXT 64
#define MAX_TEXT 128
#define MAX_LARGE_TEXT 256
#define MAX_HUGE_TEXT 512
#define MAX_TEMPLATE_TEXT 8192
#define MAX_REPORT_TEXT 16384
#define MAX_LOG_TEXT 512
#define MAX_SPOTS 2048
#define MAX_TARGETS 128
#define MAX_BANDS 16
#define MAX_VHF 16
#define MAX_SCHEDULES 32
#define MAX_SATELLITES 64
#define MAX_PASSES 32
#define MAX_RATE_LIMITS 256
#define MAX_TEMPLATE_NAME 64

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
    int id;
    int enabled;
    char label[MAX_TEXT];
    char report_kind[MAX_SMALL_TEXT];
    char hhmm[MAX_SMALL_TEXT];
    char last_fire_date[MAX_SMALL_TEXT];
} schedule_rule_t;

typedef struct {
    int id;
    int enabled;
    int norad_id;
    char name[MAX_TEXT];
    char mode_type[MAX_SMALL_TEXT];
    char notes[MAX_LARGE_TEXT];
} satellite_t;

typedef struct {
    char bind_addr[MAX_TEXT];
    int http_port;
    char admin_user[MAX_TEXT];
    char admin_password[MAX_TEXT];

    char station_name[MAX_TEXT];
    char station_grid[MAX_SMALL_TEXT];
    char psk_grids[MAX_HUGE_TEXT];
    double latitude;
    double longitude;
    double altitude_m;
    char timezone[MAX_TEXT];

    char onebot_api_base[MAX_LARGE_TEXT];
    char onebot_access_token[MAX_LARGE_TEXT];
    char onebot_webhook_token[MAX_LARGE_TEXT];
    char bot_name[MAX_TEXT];
    char bot_qq[MAX_TEXT];
    char bot_password[MAX_TEXT];
    char schedule_morning[MAX_SMALL_TEXT];
    char schedule_evening[MAX_SMALL_TEXT];
    int morning_enabled;
    int evening_enabled;
    int refresh_interval_minutes;

    int hamqsl_interval_minutes;
    int weather_interval_minutes;
    int tropo_interval_minutes;
    int meteor_interval_hours;
    int satellite_interval_hours;
    int psk_eval_interval_seconds;
    int snapshot_rebuild_seconds;

    int psk_radius_km;
    int psk_window_minutes;
    int rate_limit_per_minute;

    int geomag_alert_enabled;
    int geomag_alert_threshold_g;
    int sixm_alert_enabled;
    int sixm_alert_interval_minutes;
    int sixm_psk_trigger_spots;

    char tropo_source_url[MAX_LARGE_TEXT];
    int tropo_forecast_hours;
    int tropo_send_image;

    char meteor_source_url[MAX_LARGE_TEXT];
    int meteor_enabled;

    char satellite_source_url[MAX_LARGE_TEXT];
    char satellite_api_base[MAX_LARGE_TEXT];
    char satellite_api_key[MAX_LARGE_TEXT];
    int satellite_enabled;
    int satellite_days;
    int satellite_min_elevation;
    char satellite_window_start[MAX_SMALL_TEXT];
    char satellite_window_end[MAX_SMALL_TEXT];
    char satellite_mode_filter[MAX_SMALL_TEXT];
    int satellite_max_items;

    char hamqsl_widget_url[MAX_LARGE_TEXT];
    char hamqsl_selected_fields[MAX_HUGE_TEXT];
    int include_source_urls;
    int include_hamqsl_widget;

    char report_template_full[MAX_TEMPLATE_TEXT];
    char report_template_6m[MAX_TEMPLATE_TEXT];
    char report_template_solar[MAX_TEMPLATE_TEXT];
    char report_template_geomag[MAX_TEMPLATE_TEXT];
    char report_template_open6m[MAX_TEMPLATE_TEXT];
    char help_template[MAX_TEMPLATE_TEXT];

    char trigger_full[MAX_HUGE_TEXT];
    char trigger_6m[MAX_HUGE_TEXT];
    char trigger_solar[MAX_HUGE_TEXT];
    char trigger_help[MAX_HUGE_TEXT];
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
    char source_name[MAX_TEXT];
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
    char fof2[MAX_TEXT];
    char muffactor[MAX_TEXT];
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
    int sixm_weather_score;
    char sixm_weather_level[MAX_TEXT];
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
    char matched_grids[MAX_HUGE_TEXT];
} psk_summary_t;

typedef struct {
    int valid;
    char page_url[MAX_LARGE_TEXT];
    char image_url[MAX_LARGE_TEXT];
    int horizon_hours;
    int sample_x;
    int sample_y;
    int sample_r;
    int sample_g;
    int sample_b;
    char category[MAX_TEXT];
    int score;
    char summary[MAX_LARGE_TEXT];
} tropo_data_t;

typedef struct {
    int valid;
    char source_url[MAX_LARGE_TEXT];
    char shower_name[MAX_TEXT];
    char peak_label[MAX_TEXT];
    char peak_time[MAX_TEXT];
    int moon_percent;
    int days_left;
    char summary[MAX_LARGE_TEXT];
} meteor_data_t;

typedef struct {
    int valid;
    int norad_id;
    char name[MAX_TEXT];
    char mode_type[MAX_SMALL_TEXT];
    char start_local[MAX_TEXT];
    char max_local[MAX_TEXT];
    double max_elevation;
    char source_url[MAX_LARGE_TEXT];
} satellite_pass_t;

typedef struct {
    int valid;
    char source_url[MAX_LARGE_TEXT];
    char summary[MAX_LARGE_TEXT];
    satellite_pass_t passes[MAX_PASSES];
    int pass_count;
} satellite_summary_t;

typedef struct {
    time_t refreshed_at;
    hamqsl_data_t hamqsl;
    weather_data_t weather;
    psk_summary_t psk;
    tropo_data_t tropo;
    meteor_data_t meteor;
    satellite_summary_t satellite;

    char sun_summary[2048];
    char section_hamqsl[4096];
    char section_weather[2048];
    char section_tropo[2048];
    char section_6m[4096];
    char section_solar[2048];
    char section_meteor[2048];
    char section_satellite[4096];
    char section_sources[2048];

    char report_text[MAX_REPORT_TEXT];
    char report_6m[MAX_REPORT_TEXT];
    char report_solar[MAX_REPORT_TEXT];
    char report_geomag[MAX_REPORT_TEXT];
    char report_open6m[MAX_REPORT_TEXT];
    char report_help[MAX_REPORT_TEXT];
    char analysis_summary[2048];
} snapshot_t;

typedef struct {
    time_t next_due;
    int interval_seconds;
} poll_state_t;

typedef struct {
    char key[MAX_TEXT];
    time_t minute_window;
    int count;
} rate_limit_entry_t;

typedef struct {
    sqlite3 *db;
    pthread_mutex_t db_mutex;
    pthread_mutex_t cache_mutex;
    pthread_mutex_t refresh_mutex;
    pthread_mutex_t spot_mutex;
    pthread_mutex_t rate_mutex;

    settings_t settings;
    snapshot_t snapshot;

    psk_spot_t spots[MAX_SPOTS];
    size_t spot_head;
    size_t spot_count;
    struct mosquitto *mosq;
    int mqtt_connected;

    poll_state_t hamqsl_poll;
    poll_state_t weather_poll;
    poll_state_t tropo_poll;
    poll_state_t meteor_poll;
    poll_state_t satellite_poll;
    poll_state_t psk_eval_poll;
    poll_state_t snapshot_poll;

    int last_geomag_alert_g;
    int last_sixm_alert_level;
    time_t last_sixm_alert_at;

    rate_limit_entry_t rate_limits[MAX_RATE_LIMITS];
    int running;
    app_socket_t http_fd;
} app_t;

typedef struct {
    char *data;
    size_t size;
} memory_block_t;

typedef struct {
    char method[8];
    char path[256];
    char query[1024];
    char authorization[512];
    char content_type[128];
    char body[131072];
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
int csv_contains_ci(const char *csv, const char *needle);
double clamp_double(double value, double min_value, double max_value);
int clamp_int(int value, int min_value, int max_value);
int base64_decode(const char *input, unsigned char *output, size_t *output_len);
void app_log(app_t *app, const char *level, const char *fmt, ...);
char *http_get_text(const char *url, const char *bearer_token, long *status_code);
char *http_get_binary(const char *url, const char *bearer_token, long *status_code, size_t *out_size);
char *http_post_json(const char *url, const char *bearer_token, const char *json_body, long *status_code);
int grid_to_latlon(const char *grid, double *lat, double *lon);
double haversine_km(double lat1, double lon1, double lat2, double lon2);
int prefix_matches_grid(const char *grid, const char *prefix);
void apply_timezone(const char *tz_name);
int app_net_init(void);
void app_net_cleanup(void);

int storage_init(app_t *app, const char *db_path);
int storage_load_settings(app_t *app, settings_t *out);
int storage_save_setting(app_t *app, const char *key, const char *value);
int storage_load_targets(app_t *app, target_t *targets, int max_targets, int *out_count);
int storage_add_target(app_t *app, const target_t *target);
int storage_delete_target(app_t *app, int target_id);
int storage_toggle_target(app_t *app, int target_id, int enabled);
int storage_set_last_fire(app_t *app, const char *slot, const char *date_text);
int storage_get_last_fire(app_t *app, const char *slot, char *out, size_t out_len);
int storage_load_schedules(app_t *app, schedule_rule_t *rules, int max_rules, int *out_count);
int storage_add_schedule(app_t *app, const schedule_rule_t *rule);
int storage_delete_schedule(app_t *app, int rule_id);
int storage_toggle_schedule(app_t *app, int rule_id, int enabled);
int storage_set_schedule_last_fire(app_t *app, int rule_id, const char *date_text);
int storage_load_satellites(app_t *app, satellite_t *sats, int max_sats, int *out_count);
int storage_add_satellite(app_t *app, const satellite_t *sat);
int storage_delete_satellite(app_t *app, int sat_id);
int storage_toggle_satellite(app_t *app, int sat_id, int enabled);
int storage_get_state(app_t *app, const char *key, char *out, size_t out_len);
int storage_set_state(app_t *app, const char *key, const char *value);
int storage_load_recent_logs(app_t *app, sb_t *html_rows);

int fetch_hamqsl_data(hamqsl_data_t *out);
int fetch_weather_data(const settings_t *settings, weather_data_t *out);
int fetch_tropo_data(const settings_t *settings, tropo_data_t *out);
int fetch_meteor_data(const settings_t *settings, meteor_data_t *out);
int fetch_satellite_data(const settings_t *settings, satellite_summary_t *out, app_t *app);
void build_reports(app_t *app, snapshot_t *snapshot);
int refresh_snapshot(app_t *app, int force);
int onebot_send_message(app_t *app, target_type_t type, const char *target_id, const char *message);
int send_report_to_all_targets(app_t *app, const char *message);
int send_report_kind_to_all_targets(app_t *app, const char *report_kind);
void app_run_periodic_fetches(app_t *app);
int app_force_refresh(app_t *app);
void app_rebuild_snapshot(app_t *app);
void app_check_alerts(app_t *app);
int app_rate_limit_allow(app_t *app, const char *key);
const char *app_get_report_by_kind(const snapshot_t *snapshot, const char *kind);

int psk_start(app_t *app);
void psk_stop(app_t *app);
void psk_compute_summary(app_t *app, const settings_t *settings, psk_summary_t *out);
void psk_append_recent_rows(app_t *app, sb_t *rows, const settings_t *settings, int max_rows);

int http_server_run(app_t *app);

#endif
