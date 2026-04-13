#include "app.h"

#ifdef _WIN32
#include <objidl.h>
#include <ole2.h>
#include <gdiplus.h>
#endif

/* 这个文件处理 6m 与 2m 的实时 spots。
 * 数据来源不是网页抓取，而是直接订阅 PSKReporter 的 MQTT 流，
 * 这样更稳定，也更适合做长期运行的实时判断。 */

/* 下面几个 JSON 辅助函数是“够用型”解析器。
 * MQTT payload 的结构比较简单，所以这里不引入完整 JSON 库，
 * 只提取本项目用得到的键。 */
static const char *find_json_key_local(const char *json, const char *key) {
    static char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static int extract_json_string_local(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = find_json_key_local(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p, ':');
    if (!p) {
        return -1;
    }
    while (*p && *p != '"') {
        p++;
    }
    if (*p != '"') {
        return -1;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int extract_json_number_local(const char *json, const char *key, double *out) {
    const char *p = find_json_key_local(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p, ':');
    if (!p) {
        return -1;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    char *end = NULL;
    *out = strtod(p, &end);
    return (end != p) ? 0 : -1;
}

static int find_matching_grid_prefix(const settings_t *settings, const psk_spot_t *spot,
                                     char *matched_prefix, size_t matched_prefix_len) {
    /* 用户可以配置多个网格前缀。
     * 只要发送端或接收端命中其中任意一个前缀，就认为这条 spot 与本台相关。 */
    char grids[MAX_HUGE_TEXT];
    if (settings->psk_grids[0]) {
        copy_string(grids, sizeof(grids), settings->psk_grids);
    } else {
        copy_string(grids, sizeof(grids), settings->station_grid);
    }

    char *save = NULL;
    for (char *part = strtok_r(grids, ",|/ \t\r\n", &save); part; part = strtok_r(NULL, ",|/ \t\r\n", &save)) {
        trim_whitespace(part);
        if (!*part) {
            continue;
        }
        if (prefix_matches_grid(spot->sender_grid, part) || prefix_matches_grid(spot->receiver_grid, part)) {
            copy_string(matched_prefix, matched_prefix_len, part);
            return 1;
        }
    }
    if (matched_prefix && matched_prefix_len > 0) {
        matched_prefix[0] = '\0';
    }
    return 0;
}

static void append_unique_csv(char *csv, size_t csv_len, const char *value) {
    if (!value || !*value || csv_contains_ci(csv, value)) {
        return;
    }
    if (csv[0]) {
        strncat(csv, ",", csv_len - strlen(csv) - 1);
    }
    strncat(csv, value, csv_len - strlen(csv) - 1);
}

static void psk_add_spot(app_t *app, const psk_spot_t *spot) {
    /* spots 是固定长度环形缓冲。
     * 新 spot 到来时永远覆盖最老位置，避免程序长期运行时内存不断增长。 */
    pthread_mutex_lock(&app->spot_mutex);
    app->spots[app->spot_head] = *spot;
    app->spots[app->spot_head].in_use = 1;
    app->spot_head = (app->spot_head + 1) % MAX_SPOTS;
    if (app->spot_count < MAX_SPOTS) {
        app->spot_count++;
    }
    pthread_mutex_unlock(&app->spot_mutex);
}

static void on_connect_cb(struct mosquitto *mosq, void *userdata, int rc) {
    app_t *app = (app_t *)userdata;
    if (rc == 0) {
        /* 2m 和 6m 共用一条 MQTT 连接，减少额外连接和长期运行风险。 */
        app->mqtt_connected = 1;
        mosquitto_subscribe(mosq, NULL, "pskr/filter/v2/6m/#", 0);
        mosquitto_subscribe(mosq, NULL, "pskr/filter/v2/2m/#", 0);
        app_log(app, "INFO", "已连接 PSKReporter MQTT");
    } else {
        app->mqtt_connected = 0;
        app_log(app, "WARN", "PSKReporter MQTT 连接失败 rc=%d", rc);
    }
}

static void infer_band_from_topic(const char *topic, char *out, size_t out_len) {
    const char *p = topic ? strstr(topic, "/v2/") : NULL;
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!p) {
        return;
    }
    p += 4;
    if (strncmp(p, "6m/", 3) == 0 || strcmp(p, "6m") == 0) {
        copy_string(out, out_len, "6m");
    } else if (strncmp(p, "2m/", 3) == 0 || strcmp(p, "2m") == 0) {
        copy_string(out, out_len, "2m");
    }
}

static void on_disconnect_cb(struct mosquitto *mosq, void *userdata, int rc) {
    (void)mosq;
    app_t *app = (app_t *)userdata;
    app->mqtt_connected = 0;
    app_log(app, rc == 0 ? "INFO" : "WARN", "PSKReporter MQTT 已断开 rc=%d", rc);
}

static void on_message_cb(struct mosquitto *mosq, void *userdata, const struct mosquitto_message *msg) {
    (void)mosq;
    app_t *app = (app_t *)userdata;
    if (!msg || !msg->payload || !msg->topic) {
        return;
    }

    /* 先把 MQTT payload 还原成统一的内部结构 psk_spot_t，
     * 后续无论是摘要计算还是后台表格，都只依赖这个结构。 */
    psk_spot_t spot;
    memset(&spot, 0, sizeof(spot));
    spot.band[0] = '\0';

    const char *payload = (const char *)msg->payload;
    double number = 0.0;
    if (extract_json_number_local(payload, "f", &number) == 0) spot.frequency_hz = number;
    if (extract_json_number_local(payload, "rp", &number) == 0) spot.snr = (int)number;
    if (extract_json_number_local(payload, "t", &number) == 0) spot.timestamp = (time_t)number;
    else spot.timestamp = time(NULL);
    if (extract_json_number_local(payload, "sa", &number) == 0) spot.sender_adif = (int)number;
    if (extract_json_number_local(payload, "ra", &number) == 0) spot.receiver_adif = (int)number;
    extract_json_string_local(payload, "md", spot.mode, sizeof(spot.mode));
    extract_json_string_local(payload, "sc", spot.sender_call, sizeof(spot.sender_call));
    extract_json_string_local(payload, "sl", spot.sender_grid, sizeof(spot.sender_grid));
    extract_json_string_local(payload, "rc", spot.receiver_call, sizeof(spot.receiver_call));
    extract_json_string_local(payload, "rl", spot.receiver_grid, sizeof(spot.receiver_grid));
    extract_json_string_local(payload, "b", spot.band, sizeof(spot.band));
    if (!spot.band[0]) {
        infer_band_from_topic((const char *)msg->topic, spot.band, sizeof(spot.band));
    }
    if (!spot.timestamp) {
        spot.timestamp = time(NULL);
    }
    if (!spot.band[0]) {
        copy_string(spot.band, sizeof(spot.band), "6m");
    }
    psk_add_spot(app, &spot);
}

int psk_start(app_t *app) {
    /* MQTT 用异步连接 + 后台 loop 线程。
     * 主线程不用自己轮询 socket，适合常驻服务。 */
    mosquitto_lib_init();
    app->mosq = mosquitto_new("propagation-forecast-bot", true, app);
    if (!app->mosq) {
        return -1;
    }
    mosquitto_reconnect_delay_set(app->mosq, 2, 30, true);
    mosquitto_connect_callback_set(app->mosq, on_connect_cb);
    mosquitto_disconnect_callback_set(app->mosq, on_disconnect_cb);
    mosquitto_message_callback_set(app->mosq, on_message_cb);

    int rc = mosquitto_connect_async(app->mosq, "mqtt.pskreporter.info", 1883, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        app_log(app, "ERROR", "无法连接 PSKReporter MQTT: %s", mosquitto_strerror(rc));
        mosquitto_destroy(app->mosq);
        app->mosq = NULL;
        return -1;
    }
    rc = mosquitto_loop_start(app->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        app_log(app, "ERROR", "无法启动 MQTT 循环: %s", mosquitto_strerror(rc));
        mosquitto_destroy(app->mosq);
        app->mosq = NULL;
        return -1;
    }
    return 0;
}

void psk_stop(app_t *app) {
    if (!app->mosq) {
        return;
    }
    mosquitto_disconnect(app->mosq);
    mosquitto_loop_stop(app->mosq, true);
    mosquitto_destroy(app->mosq);
    app->mosq = NULL;
    mosquitto_lib_cleanup();
}

static int is_local_relevant(const settings_t *settings, const psk_spot_t *spot,
                             double station_lat, double station_lon,
                             int *counterpart_distance, char *peer_call, size_t peer_call_len,
                             char *peer_grid, size_t peer_grid_len,
                             char *matched_prefix, size_t matched_prefix_len,
                             double *peer_lat, double *peer_lon) {
    /* “是否与本地有关”有两条路：
     * 1. 直接命中用户配置的网格前缀
     * 2. 通过经纬度计算，落在用户设定半径内
     *
     * 命中后还会顺手算出对端距离，方便后面做 6m 强度分析和后台展示。 */
    int station_has_coords = !(station_lat == 0.0 && station_lon == 0.0);
    int direct_match = find_matching_grid_prefix(settings, spot, matched_prefix, matched_prefix_len);
    double sender_lat = 0.0, sender_lon = 0.0, receiver_lat = 0.0, receiver_lon = 0.0;
    int sender_ok = grid_to_latlon(spot->sender_grid, &sender_lat, &sender_lon) == 0;
    int receiver_ok = grid_to_latlon(spot->receiver_grid, &receiver_lat, &receiver_lon) == 0;
    int sender_near = 0;
    int receiver_near = 0;
    if (station_has_coords && sender_ok) {
        sender_near = haversine_km(station_lat, station_lon, sender_lat, sender_lon) <= settings->psk_radius_km;
    }
    if (station_has_coords && receiver_ok) {
        receiver_near = haversine_km(station_lat, station_lon, receiver_lat, receiver_lon) <= settings->psk_radius_km;
    }
    if (!direct_match && !sender_near && !receiver_near) {
        return 0;
    }

    *counterpart_distance = 0;
    if (peer_lat) {
        *peer_lat = 0.0;
    }
    if (peer_lon) {
        *peer_lon = 0.0;
    }
    if ((direct_match || sender_near) && receiver_ok) {
        *counterpart_distance = (int)round(haversine_km(station_lat, station_lon, receiver_lat, receiver_lon));
        copy_string(peer_call, peer_call_len, spot->receiver_call);
        copy_string(peer_grid, peer_grid_len, spot->receiver_grid);
        if (peer_lat) {
            *peer_lat = receiver_lat;
        }
        if (peer_lon) {
            *peer_lon = receiver_lon;
        }
    } else if ((direct_match || receiver_near) && sender_ok) {
        *counterpart_distance = (int)round(haversine_km(station_lat, station_lon, sender_lat, sender_lon));
        copy_string(peer_call, peer_call_len, spot->sender_call);
        copy_string(peer_grid, peer_grid_len, spot->sender_grid);
        if (peer_lat) {
            *peer_lat = sender_lat;
        }
        if (peer_lon) {
            *peer_lon = sender_lon;
        }
    }
    return 1;
}

void psk_compute_summary_for_band(app_t *app, const settings_t *settings, const char *band, psk_summary_t *out) {
    /* 同一套统计逻辑同时服务 6m 与 2m，只在 band 过滤条件上不同。 */
    memset(out, 0, sizeof(*out));
    out->mqtt_connected = app->mqtt_connected;
    out->best_snr = -999;

    double station_lat = settings->latitude;
    double station_lon = settings->longitude;
    if ((station_lat == 0.0 && station_lon == 0.0) || isnan(station_lat) || isnan(station_lon)) {
        if (grid_to_latlon(settings->station_grid, &station_lat, &station_lon) != 0 && settings->psk_grids[0]) {
            char first_grid[MAX_TEXT];
            copy_string(first_grid, sizeof(first_grid), settings->psk_grids);
            char *sep = strpbrk(first_grid, ",|/ \t\r\n");
            if (sep) *sep = '\0';
            if (grid_to_latlon(first_grid, &station_lat, &station_lon) != 0) {
                station_lat = 0.0;
                station_lon = 0.0;
            }
        }
    }

    time_t now = time(NULL);
    time_t cutoff_15 = now - 15 * 60;
    time_t cutoff_60 = now - settings->psk_window_minutes * 60;
    time_t latest_local_ts = 0;

    pthread_mutex_lock(&app->spot_mutex);
    for (size_t i = 0; i < app->spot_count; ++i) {
        const psk_spot_t *spot = &app->spots[i];
        if (!spot->in_use || spot->timestamp < cutoff_60) {
            continue;
        }
        if (band && *band && strcasecmp(spot->band, band) != 0) {
            continue;
        }

        out->global_spots_60m++;
        if (spot->timestamp >= cutoff_15) {
            out->global_spots_15m++;
        }

        int peer_distance = 0;
        char peer_call[MAX_TEXT] = {0};
        char peer_grid[MAX_TEXT] = {0};
        char matched_prefix[MAX_TEXT] = {0};
        int relevant = (settings->psk_grids[0] || settings->station_grid[0]) &&
            is_local_relevant(settings, spot, station_lat, station_lon, &peer_distance,
                peer_call, sizeof(peer_call), peer_grid, sizeof(peer_grid),
                matched_prefix, sizeof(matched_prefix), NULL, NULL);
        if (!relevant) {
            continue;
        }

        out->local_spots_60m++;
        if (spot->timestamp >= cutoff_15) {
            out->local_spots_15m++;
        }
        if (spot->snr > out->best_snr) {
            out->best_snr = spot->snr;
        }
        if (peer_distance > out->longest_path_km) {
            out->longest_path_km = peer_distance;
            copy_string(out->farthest_peer, sizeof(out->farthest_peer), peer_call);
            copy_string(out->farthest_grid, sizeof(out->farthest_grid), peer_grid);
        }
        if (spot->timestamp >= latest_local_ts) {
            latest_local_ts = spot->timestamp;
            format_time_local(spot->timestamp, out->latest_local_time, sizeof(out->latest_local_time));
            snprintf(out->latest_pair, sizeof(out->latest_pair), "%s(%s) -> %s(%s) %s %d dB",
                spot->sender_call, spot->sender_grid,
                spot->receiver_call, spot->receiver_grid,
                spot->mode, spot->snr);
        }
        append_unique_csv(out->matched_grids, sizeof(out->matched_grids), matched_prefix);
    }
    pthread_mutex_unlock(&app->spot_mutex);

    {
        int score = 0;
        score += out->mqtt_connected ? 8 : 0;
        score += clamp_int(out->local_spots_15m * 18, 0, 45);
        score += clamp_int(out->local_spots_60m * 8, 0, 28);
        if (out->longest_path_km >= 1200) {
            score += 12;
        } else if (out->longest_path_km >= 600) {
            score += 6;
        }
        if (out->global_spots_60m >= 100) {
            score += 7;
        } else if (out->global_spots_60m >= 30) {
            score += 4;
        }
        out->score = clamp_int(score, 0, 100);
    }

    if (out->local_spots_15m >= 3 || out->local_spots_60m >= 6) {
        copy_string(out->assessment, sizeof(out->assessment),
            settings->wording_psk_assessment_open[0] ? settings->wording_psk_assessment_open : "明确开口");
        copy_string(out->confidence, sizeof(out->confidence),
            settings->wording_psk_confidence_high[0] ? settings->wording_psk_confidence_high : "高");
    } else if (out->local_spots_60m >= 2) {
        copy_string(out->assessment, sizeof(out->assessment),
            settings->wording_psk_assessment_possible[0] ? settings->wording_psk_assessment_possible : "有开口迹象");
        copy_string(out->confidence, sizeof(out->confidence),
            settings->wording_psk_confidence_medium[0] ? settings->wording_psk_confidence_medium : "中");
    } else if (out->global_spots_60m >= 20) {
        copy_string(out->assessment, sizeof(out->assessment),
            settings->wording_psk_assessment_global_only[0] ? settings->wording_psk_assessment_global_only : "全球活跃，本地待观察");
        copy_string(out->confidence, sizeof(out->confidence),
            settings->wording_psk_confidence_low[0] ? settings->wording_psk_confidence_low : "低");
    } else if (out->mqtt_connected) {
        copy_string(out->assessment, sizeof(out->assessment),
            settings->wording_psk_assessment_quiet[0] ? settings->wording_psk_assessment_quiet : "暂未见本地开口");
        copy_string(out->confidence, sizeof(out->confidence),
            settings->wording_psk_confidence_low[0] ? settings->wording_psk_confidence_low : "低");
    } else {
        copy_string(out->assessment, sizeof(out->assessment),
            settings->wording_psk_assessment_disconnected[0] ? settings->wording_psk_assessment_disconnected : "实时数据未连接");
        copy_string(out->confidence, sizeof(out->confidence),
            settings->wording_psk_confidence_unknown[0] ? settings->wording_psk_confidence_unknown : "未知");
    }
}

void psk_compute_summary(app_t *app, const settings_t *settings, psk_summary_t *out) {
    psk_compute_summary_for_band(app, settings, "6m", out);
}

void psk_append_recent_rows_for_band(app_t *app, sb_t *rows, const settings_t *settings, const char *band, int max_rows) {
    /* 后台最近 spot 表格按时间倒序展示，只显示最近窗口内且与本地相关的指定波段 spot。 */
    double station_lat = settings->latitude;
    double station_lon = settings->longitude;
    if ((station_lat == 0.0 && station_lon == 0.0) || isnan(station_lat) || isnan(station_lon)) {
        grid_to_latlon(settings->station_grid, &station_lat, &station_lon);
    }

    time_t cutoff = time(NULL) - settings->psk_window_minutes * 60;
    int emitted = 0;
    pthread_mutex_lock(&app->spot_mutex);
    for (size_t step = 0; step < app->spot_count && emitted < max_rows; ++step) {
        size_t idx = (app->spot_head + MAX_SPOTS - 1 - step) % MAX_SPOTS;
        const psk_spot_t *spot = &app->spots[idx];
        if (!spot->in_use || spot->timestamp < cutoff) {
            continue;
        }
        if (band && *band && strcasecmp(spot->band, band) != 0) {
            continue;
        }
        int peer_distance = 0;
        char peer_call[MAX_TEXT] = {0};
        char peer_grid[MAX_TEXT] = {0};
        char matched_prefix[MAX_TEXT] = {0};
        if (!is_local_relevant(settings, spot, station_lat, station_lon,
                &peer_distance, peer_call, sizeof(peer_call), peer_grid, sizeof(peer_grid),
                matched_prefix, sizeof(matched_prefix), NULL, NULL)) {
            continue;
        }
        char ts[64];
        format_time_local(spot->timestamp, ts, sizeof(ts));
        sb_append(rows, "<tr><td>");
        html_escape_to_sb(rows, ts);
        sb_append(rows, "</td><td>");
        html_escape_to_sb(rows, matched_prefix[0] ? matched_prefix : "-");
        sb_append(rows, "</td><td>");
        html_escape_to_sb(rows, spot->sender_call);
        sb_append(rows, " ");
        html_escape_to_sb(rows, spot->sender_grid);
        sb_append(rows, "</td><td>");
        html_escape_to_sb(rows, spot->receiver_call);
        sb_append(rows, " ");
        html_escape_to_sb(rows, spot->receiver_grid);
        sb_append(rows, "</td><td>");
        html_escape_to_sb(rows, spot->mode);
        sb_append(rows, "</td><td>");
        sb_appendf(rows, "%d", spot->snr);
        sb_append(rows, "</td><td>");
        sb_appendf(rows, "%d km", peer_distance);
        sb_append(rows, "</td></tr>");
        emitted++;
    }
    pthread_mutex_unlock(&app->spot_mutex);

    if (emitted == 0) {
        sb_appendf(rows, "<tr><td colspan=\"7\">最近窗口内还没有本地相关 %s spot</td></tr>",
            band && *band ? band : "VHF");
    }
}

void psk_append_recent_rows(app_t *app, sb_t *rows, const settings_t *settings, int max_rows) {
    psk_append_recent_rows_for_band(app, rows, settings, "6m", max_rows);
}

typedef struct {
    time_t timestamp;
    int snr;
    int distance_km;
    int has_peer_coords;
    double peer_lat;
    double peer_lon;
    char sender_call[MAX_TEXT];
    char sender_grid[MAX_TEXT];
    char receiver_call[MAX_TEXT];
    char receiver_grid[MAX_TEXT];
    char mode[MAX_SMALL_TEXT];
    char matched_prefix[MAX_TEXT];
} psk_map_spot_t;

typedef struct {
    double min_lat;
    double max_lat;
    double min_lon;
    double max_lon;
    int initialized;
} psk_map_bounds_t;

static void resolve_station_coords(const settings_t *settings, double *station_lat, double *station_lon) {
    *station_lat = settings->latitude;
    *station_lon = settings->longitude;
    if (((*station_lat == 0.0 && *station_lon == 0.0) || isnan(*station_lat) || isnan(*station_lon)) &&
        settings->station_grid[0]) {
        if (grid_to_latlon(settings->station_grid, station_lat, station_lon) != 0 && settings->psk_grids[0]) {
            char first_grid[MAX_TEXT];
            copy_string(first_grid, sizeof(first_grid), settings->psk_grids);
            char *sep = strpbrk(first_grid, ",|/ \t\r\n");
            if (sep) {
                *sep = '\0';
            }
            if (grid_to_latlon(first_grid, station_lat, station_lon) != 0) {
                *station_lat = 31.2304;
                *station_lon = 121.4737;
            }
        }
    }
}

static const char *normalize_psk_band(const char *band) {
    return (band && strcasecmp(band, "2m") == 0) ? "2m" : "6m";
}

static const psk_summary_t *summary_for_band(const snapshot_t *snapshot, const char *band) {
    return (strcasecmp(normalize_psk_band(band), "2m") == 0) ? &snapshot->twom : &snapshot->psk;
}

static const char *label_for_band(const char *band) {
    return (strcasecmp(normalize_psk_band(band), "2m") == 0) ? "2m" : "6m";
}

static const char *mhz_label_for_band(const char *band) {
    return (strcasecmp(normalize_psk_band(band), "2m") == 0) ? "144 MHz" : "50 MHz";
}

static const char *alert_label_for_band(const snapshot_t *snapshot, const settings_t *settings, const char *band) {
    return (strcasecmp(normalize_psk_band(band), "2m") == 0) ?
        twom_alert_label_from_snapshot(snapshot, settings) :
        sixm_alert_label_from_snapshot(snapshot, settings);
}

static const char *alert_code_for_band(const snapshot_t *snapshot, const settings_t *settings, const char *band) {
    return (strcasecmp(normalize_psk_band(band), "2m") == 0) ?
        twom_alert_code_from_snapshot(snapshot, settings) :
        sixm_alert_code_from_snapshot(snapshot, settings);
}

static int alert_num_for_band(const snapshot_t *snapshot, const settings_t *settings, const char *band) {
    return (strcasecmp(normalize_psk_band(band), "2m") == 0) ?
        twom_alert_num_from_snapshot(snapshot, settings) :
        sixm_alert_num_from_snapshot(snapshot, settings);
}

static int collect_recent_map_spots(app_t *app, const settings_t *settings, const char *band,
                                    psk_map_spot_t *spots, int max_spots,
                                    double *station_lat, double *station_lon) {
    const char *normalized_band = normalize_psk_band(band);
    time_t cutoff = time(NULL) - settings->psk_window_minutes * 60;
    int count = 0;
    resolve_station_coords(settings, station_lat, station_lon);

    pthread_mutex_lock(&app->spot_mutex);
    for (size_t step = 0; step < app->spot_count && count < max_spots; ++step) {
        size_t idx = (app->spot_head + MAX_SPOTS - 1 - step) % MAX_SPOTS;
        const psk_spot_t *spot = &app->spots[idx];
        int peer_distance = 0;
        double peer_lat = 0.0;
        double peer_lon = 0.0;
        char peer_call[MAX_TEXT] = {0};
        char peer_grid[MAX_TEXT] = {0};
        char matched_prefix[MAX_TEXT] = {0};

        if (!spot->in_use || spot->timestamp < cutoff) {
            continue;
        }
        if (strcasecmp(spot->band, normalized_band) != 0) {
            continue;
        }
        if (!is_local_relevant(settings, spot, *station_lat, *station_lon,
                &peer_distance, peer_call, sizeof(peer_call), peer_grid, sizeof(peer_grid),
                matched_prefix, sizeof(matched_prefix), &peer_lat, &peer_lon)) {
            continue;
        }

        memset(&spots[count], 0, sizeof(spots[count]));
        spots[count].timestamp = spot->timestamp;
        spots[count].snr = spot->snr;
        spots[count].distance_km = peer_distance;
        spots[count].has_peer_coords = !(peer_lat == 0.0 && peer_lon == 0.0);
        spots[count].peer_lat = peer_lat;
        spots[count].peer_lon = peer_lon;
        copy_string(spots[count].sender_call, sizeof(spots[count].sender_call), spot->sender_call);
        copy_string(spots[count].sender_grid, sizeof(spots[count].sender_grid), spot->sender_grid);
        copy_string(spots[count].receiver_call, sizeof(spots[count].receiver_call), spot->receiver_call);
        copy_string(spots[count].receiver_grid, sizeof(spots[count].receiver_grid), spot->receiver_grid);
        copy_string(spots[count].mode, sizeof(spots[count].mode), spot->mode);
        copy_string(spots[count].matched_prefix, sizeof(spots[count].matched_prefix), matched_prefix);
        count++;
    }
    pthread_mutex_unlock(&app->spot_mutex);
    return count;
}

static void bounds_include(psk_map_bounds_t *bounds, double lat, double lon) {
    if (isnan(lat) || isnan(lon)) {
        return;
    }
    if (!bounds->initialized) {
        bounds->min_lat = bounds->max_lat = lat;
        bounds->min_lon = bounds->max_lon = lon;
        bounds->initialized = 1;
        return;
    }
    if (lat < bounds->min_lat) bounds->min_lat = lat;
    if (lat > bounds->max_lat) bounds->max_lat = lat;
    if (lon < bounds->min_lon) bounds->min_lon = lon;
    if (lon > bounds->max_lon) bounds->max_lon = lon;
}

static void finalize_bounds(psk_map_bounds_t *bounds, double station_lat, double station_lon) {
    double lat_range;
    double lon_range;
    if (!bounds->initialized) {
        bounds->initialized = 1;
        bounds->min_lat = station_lat - 8.0;
        bounds->max_lat = station_lat + 8.0;
        bounds->min_lon = station_lon - 12.0;
        bounds->max_lon = station_lon + 12.0;
    }
    lat_range = bounds->max_lat - bounds->min_lat;
    lon_range = bounds->max_lon - bounds->min_lon;
    if (lat_range < 8.0) {
        double pad = (8.0 - lat_range) / 2.0;
        bounds->min_lat -= pad;
        bounds->max_lat += pad;
    }
    if (lon_range < 12.0) {
        double pad = (12.0 - lon_range) / 2.0;
        bounds->min_lon -= pad;
        bounds->max_lon += pad;
    }
    lat_range = bounds->max_lat - bounds->min_lat;
    lon_range = bounds->max_lon - bounds->min_lon;
    bounds->min_lat = clamp_double(bounds->min_lat - lat_range * 0.10 - 0.5, -85.0, 85.0);
    bounds->max_lat = clamp_double(bounds->max_lat + lat_range * 0.10 + 0.5, -85.0, 85.0);
    bounds->min_lon = clamp_double(bounds->min_lon - lon_range * 0.10 - 0.5, -180.0, 180.0);
    bounds->max_lon = clamp_double(bounds->max_lon + lon_range * 0.10 + 0.5, -180.0, 180.0);
}

static double axis_step(double range) {
    if (range > 80.0) return 20.0;
    if (range > 40.0) return 10.0;
    if (range > 20.0) return 5.0;
    if (range > 10.0) return 2.0;
    return 1.0;
}

static double lon_to_x(double lon, const psk_map_bounds_t *bounds, double left, double width) {
    double range = bounds->max_lon - bounds->min_lon;
    if (range <= 0.0) {
        return left + width / 2.0;
    }
    return left + (lon - bounds->min_lon) / range * width;
}

static double lat_to_y(double lat, const psk_map_bounds_t *bounds, double top, double height) {
    double range = bounds->max_lat - bounds->min_lat;
    if (range <= 0.0) {
        return top + height / 2.0;
    }
    return top + (bounds->max_lat - lat) / range * height;
}

static void format_axis_label(double value, int is_lon, char *out, size_t out_len) {
    char suffix = 'E';
    double shown = value;
    if (is_lon) {
        if (value < 0.0) {
            suffix = 'W';
            shown = fabs(value);
        }
    } else if (value < 0.0) {
        suffix = 'S';
        shown = fabs(value);
    } else {
        suffix = 'N';
    }
    snprintf(out, out_len, "%.0f°%c", shown, suffix);
}

static void render_psk_snapshot_html(sb_t *html, const settings_t *settings, const snapshot_t *snapshot,
                                     const psk_map_spot_t *spots, int spot_count,
                                     double station_lat, double station_lon) {
    const double svg_w = 860.0;
    const double svg_h = 560.0;
    const double plot_left = 74.0;
    const double plot_top = 36.0;
    const double plot_w = 752.0;
    const double plot_h = 476.0;
    psk_map_bounds_t bounds;
    char generated_at[MAX_TEXT];
    time_t now = time(NULL);

    memset(&bounds, 0, sizeof(bounds));
    bounds_include(&bounds, station_lat, station_lon);
    for (int i = 0; i < spot_count; ++i) {
        if (spots[i].has_peer_coords) {
            bounds_include(&bounds, spots[i].peer_lat, spots[i].peer_lon);
        }
    }
    finalize_bounds(&bounds, station_lat, station_lon);
    format_time_local(now, generated_at, sizeof(generated_at));

    sb_append(html,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<style>"
        "html,body{margin:0;background:#f7efe2;font-family:\"Microsoft YaHei UI\",\"Segoe UI\",sans-serif;color:#2b241f;}"
        ".frame{width:1360px;min-height:940px;box-sizing:border-box;padding:30px 34px;background:"
        "radial-gradient(circle at top left,#fff9ef,#f4e5cd 58%,#e8d6bf 100%);}"
        ".hero{display:flex;justify-content:space-between;align-items:flex-start;gap:20px;margin-bottom:18px;}"
        ".title{font-size:34px;font-weight:800;letter-spacing:.04em;margin:0 0 6px 0;}"
        ".subtitle{font-size:15px;color:#6b5d52;margin:0;line-height:1.5;}"
        ".stats{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:12px;margin-bottom:18px;}"
        ".stat{background:rgba(255,255,255,.78);border:1px solid rgba(88,60,38,.12);border-radius:18px;padding:14px 16px;"
        "box-shadow:0 10px 24px rgba(88,60,38,.08);}"
        ".stat b{display:block;font-size:13px;color:#8d6f56;margin-bottom:8px;}"
        ".stat span{font-size:24px;font-weight:800;color:#2f2218;}"
        ".layout{display:grid;grid-template-columns:2fr 1fr;gap:18px;}"
        ".panel{background:rgba(255,255,255,.84);border:1px solid rgba(88,60,38,.12);border-radius:24px;padding:18px;"
        "box-shadow:0 18px 34px rgba(88,60,38,.10);}"
        ".panel h2{margin:0 0 12px 0;font-size:20px;}"
        ".muted{color:#7a695e;font-size:13px;line-height:1.5;}"
        ".spot-table{width:100%;border-collapse:collapse;font-size:13px;}"
        ".spot-table th,.spot-table td{padding:8px 6px;border-bottom:1px solid rgba(91,67,49,.10);text-align:left;}"
        ".spot-table th{font-size:12px;color:#8c725a;letter-spacing:.04em;text-transform:uppercase;}"
        ".footer{margin-top:14px;color:#7a695e;font-size:12px;line-height:1.55;}"
        ".badge{display:inline-block;padding:7px 11px;border-radius:999px;background:#1d776f;color:#fff;font-size:12px;font-weight:700;}"
        "svg{width:100%;height:auto;display:block;border-radius:18px;background:linear-gradient(180deg,#fff9f1,#f7efe4);}"
        ".legend{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px;font-size:12px;color:#6b5d52;}"
        ".legend i{display:inline-block;width:12px;height:12px;border-radius:999px;vertical-align:middle;margin-right:4px;}"
        "</style></head><body><div class=\"frame\">");

    sb_append(html, "<div class=\"hero\"><div><h1 class=\"title\">PSKReporter 6m 快照</h1><p class=\"subtitle\">");
    sb_appendf(html, "台站 %s · 主网格 %s · 监控网格 %s · 生成于 %s",
        settings->station_name[0] ? settings->station_name : "-",
        settings->station_grid[0] ? settings->station_grid : "-",
        settings->psk_grids[0] ? settings->psk_grids : settings->station_grid,
        generated_at);
    sb_append(html, "</p></div><div class=\"badge\">");
    html_escape_to_sb(html, snapshot->psk.assessment[0] ? snapshot->psk.assessment : "等待数据");
    sb_append(html, "</div></div>");

    sb_append(html, "<div class=\"stats\">");
    sb_appendf(html, "<div class=\"stat\"><b>15 分钟本地命中</b><span>%d</span></div>", snapshot->psk.local_spots_15m);
    sb_appendf(html, "<div class=\"stat\"><b>%d 分钟本地命中</b><span>%d</span></div>",
        settings->psk_window_minutes, snapshot->psk.local_spots_60m);
    sb_appendf(html, "<div class=\"stat\"><b>最长路径</b><span>%d km</span></div>", snapshot->psk.longest_path_km);
    sb_appendf(html, "<div class=\"stat\"><b>最佳 SNR</b><span>%d dB</span></div>", snapshot->psk.best_snr > -999 ? snapshot->psk.best_snr : 0);
    sb_appendf(html, "<div class=\"stat\"><b>判断置信度</b><span>%s</span></div>",
        snapshot->psk.confidence[0] ? snapshot->psk.confidence : "未知");
    sb_append(html, "</div>");

    sb_append(html, "<div class=\"layout\"><div class=\"panel\"><h2>路径图</h2>");
    sb_appendf(html, "<svg viewBox=\"0 0 %.0f %.0f\" xmlns=\"http://www.w3.org/2000/svg\">", svg_w, svg_h);
    sb_appendf(html, "<rect x=\"0\" y=\"0\" width=\"%.0f\" height=\"%.0f\" rx=\"18\" fill=\"#fff7ee\"/>", svg_w, svg_h);
    sb_appendf(html, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"16\" fill=\"#fffdf8\" stroke=\"#dcc8b1\" stroke-width=\"1.2\"/>",
        plot_left, plot_top, plot_w, plot_h);

    {
        double lon_step = axis_step(bounds.max_lon - bounds.min_lon);
        double lat_step = axis_step(bounds.max_lat - bounds.min_lat);
        double lon_value = floor(bounds.min_lon / lon_step) * lon_step;
        double lat_value = floor(bounds.min_lat / lat_step) * lat_step;

        for (; lon_value <= bounds.max_lon + 0.001; lon_value += lon_step) {
            double x = lon_to_x(lon_value, &bounds, plot_left, plot_w);
            char label[32];
            format_axis_label(lon_value, 1, label, sizeof(label));
            sb_appendf(html,
                "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#eadbcc\" stroke-width=\"1\"/>"
                "<text x=\"%.1f\" y=\"%.1f\" font-size=\"11\" fill=\"#8f765d\" text-anchor=\"middle\">%s</text>",
                x, plot_top, x, plot_top + plot_h, x, plot_top + plot_h + 18.0, label);
        }
        for (; lat_value <= bounds.max_lat + 0.001; lat_value += lat_step) {
            double y = lat_to_y(lat_value, &bounds, plot_top, plot_h);
            char label[32];
            format_axis_label(lat_value, 0, label, sizeof(label));
            sb_appendf(html,
                "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"#eadbcc\" stroke-width=\"1\"/>"
                "<text x=\"%.1f\" y=\"%.1f\" font-size=\"11\" fill=\"#8f765d\" text-anchor=\"end\">%s</text>",
                plot_left, y, plot_left + plot_w, y, plot_left - 10.0, y + 4.0, label);
        }
    }

    {
        double sx = lon_to_x(station_lon, &bounds, plot_left, plot_w);
        double sy = lat_to_y(station_lat, &bounds, plot_top, plot_h);
        for (int i = spot_count - 1; i >= 0; --i) {
            if (!spots[i].has_peer_coords) {
                continue;
            }
            double px = lon_to_x(spots[i].peer_lon, &bounds, plot_left, plot_w);
            double py = lat_to_y(spots[i].peer_lat, &bounds, plot_top, plot_h);
            double age_minutes = difftime(now, spots[i].timestamp) / 60.0;
            double age_ratio = clamp_double(age_minutes / (double)clamp_int(settings->psk_window_minutes, 15, 720), 0.0, 1.0);
            double line_width = 1.6 + clamp_double((spots[i].snr + 20) / 18.0, 0.0, 3.2);
            int red = 233 - (int)(age_ratio * 96.0);
            int green = 118 + (int)(age_ratio * 52.0);
            int blue = 71 + (int)(age_ratio * 104.0);
            sb_appendf(html,
                "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"rgba(%d,%d,%d,0.70)\" stroke-width=\"%.2f\" stroke-linecap=\"round\"/>",
                sx, sy, px, py, red, green, blue, line_width);
            sb_appendf(html,
                "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"6.0\" fill=\"rgba(%d,%d,%d,0.92)\" stroke=\"#fff\" stroke-width=\"1.5\"/>",
                px, py, red, green, blue);
        }
        sb_appendf(html,
            "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"18\" fill=\"rgba(196,70,44,0.16)\"/>"
            "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"8.5\" fill=\"#c4462c\" stroke=\"#fff7f0\" stroke-width=\"2.4\"/>"
            "<text x=\"%.1f\" y=\"%.1f\" font-size=\"12\" fill=\"#6f3d2b\" font-weight=\"700\">本站</text>",
            sx, sy, sx, sy, sx + 12.0, sy - 12.0);
    }

    sb_append(html, "</svg><div class=\"legend\">");
    sb_append(html, "<span><i style=\"background:#c4462c\"></i>本站</span>");
    sb_append(html, "<span><i style=\"background:#e97647\"></i>较新命中</span>");
    sb_append(html, "<span><i style=\"background:#7d88af\"></i>较早命中</span>");
    sb_append(html, "</div></div>");

    sb_append(html, "<div class=\"panel\"><h2>最近命中</h2><table class=\"spot-table\"><tr><th>时间</th><th>路径</th><th>模式</th><th>SNR</th><th>距离</th></tr>");
    if (spot_count == 0) {
        sb_append(html, "<tr><td colspan=\"5\">最近窗口内还没有与本地网格相关的 6m 命中。</td></tr>");
    } else {
        for (int i = 0; i < spot_count; ++i) {
            char ts[MAX_TEXT];
            format_time_local(spots[i].timestamp, ts, sizeof(ts));
            sb_append(html, "<tr><td>");
            html_escape_to_sb(html, ts);
            sb_append(html, "</td><td>");
            html_escape_to_sb(html, spots[i].sender_call);
            sb_append(html, " ");
            html_escape_to_sb(html, spots[i].sender_grid);
            sb_append(html, " → ");
            html_escape_to_sb(html, spots[i].receiver_call);
            sb_append(html, " ");
            html_escape_to_sb(html, spots[i].receiver_grid);
            sb_append(html, "</td><td>");
            html_escape_to_sb(html, spots[i].mode);
            sb_append(html, "</td><td>");
            sb_appendf(html, "%d", spots[i].snr);
            sb_append(html, "</td><td>");
            sb_appendf(html, "%d km", spots[i].distance_km);
            sb_append(html, "</td></tr>");
        }
    }
    sb_append(html, "</table>");
    sb_append(html, "<p class=\"muted\">判断：");
    html_escape_to_sb(html, snapshot->psk.assessment[0] ? snapshot->psk.assessment : "等待数据");
    sb_append(html, "；命中网格：");
    html_escape_to_sb(html, snapshot->psk.matched_grids[0] ? snapshot->psk.matched_grids : "暂无");
    sb_append(html, "；最新链路：");
    html_escape_to_sb(html, snapshot->psk.latest_pair[0] ? snapshot->psk.latest_pair : "暂无");
    sb_append(html, "</p><div class=\"footer\">");
    sb_append(html, "本图按 PSKReporter 实时 6m MQTT spot 自动生成，避免网页临时安全验证导致的抓图失败。<br>");
    sb_append(html, "如需对照网页地图，可手动打开 https://pskreporter.info/pskmap.html 查看。");
    sb_append(html, "</div></div></div></div></body></html>");
}

int psk_send_snapshot_image(app_t *app, target_type_t type, const char *target_id) {
    settings_t settings;
    snapshot_t snapshot;
    psk_map_spot_t spots[12];
    int spot_count = 0;
    double station_lat = 0.0;
    double station_lon = 0.0;
    unsigned char *png_data = NULL;
    size_t png_size = 0;
    char detail[256];
    sb_t html;
    sb_t message;
    char *base64 = NULL;
    char local_spots_15m_text[32];
    char local_spots_60m_text[32];
    char psk_window_text[32];
    char page_url[MAX_LARGE_TEXT];
    int rc = -1;

    if (!app || !target_id || !*target_id) {
        return -1;
    }

    app_rebuild_snapshot(app);
    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    snprintf(local_spots_15m_text, sizeof(local_spots_15m_text), "%d", snapshot.psk.local_spots_15m);
    snprintf(local_spots_60m_text, sizeof(local_spots_60m_text), "%d", snapshot.psk.local_spots_60m);
    snprintf(psk_window_text, sizeof(psk_window_text), "%d", settings.psk_window_minutes);
    copy_string(page_url, sizeof(page_url), "https://pskreporter.info/pskmap.html");

    spot_count = collect_recent_map_spots(app, &settings, "6m", spots,
        (int)(sizeof(spots) / sizeof(spots[0])), &station_lat, &station_lon);

    {
        char global_spots_15m_text[32];
        char global_spots_60m_text[32];
        char longest_path_text[32];
        char best_snr_text[32];
        char psk_score_text[32];
        char pskmap_spot_count_text[32];
        char sixm_alert_level_num_text[32];
        const char *assessment = snapshot.psk.assessment[0] ? snapshot.psk.assessment :
            (settings.wording_psk_assessment_disconnected[0] ? settings.wording_psk_assessment_disconnected : "实时数据未连接");
        const char *confidence = snapshot.psk.confidence[0] ? snapshot.psk.confidence :
            (settings.wording_psk_confidence_unknown[0] ? settings.wording_psk_confidence_unknown : "未知");
        const char *latest_pair = snapshot.psk.latest_pair[0] ? snapshot.psk.latest_pair : "暂无";
        const char *matched_grids = snapshot.psk.matched_grids[0] ? snapshot.psk.matched_grids : "暂无";
        const char *farthest_peer = snapshot.psk.farthest_peer[0] ? snapshot.psk.farthest_peer : "暂无";
        const char *farthest_grid = snapshot.psk.farthest_grid[0] ? snapshot.psk.farthest_grid : "暂无";
        const char *assessment_code = psk_assessment_code_from_summary(&snapshot.psk);
        const char *confidence_code = psk_confidence_code_from_summary(&snapshot.psk);
        const char *alert_label = sixm_alert_label_from_snapshot(&snapshot, &settings);
        const char *alert_code = sixm_alert_code_from_snapshot(&snapshot, &settings);
        char *rendered_message = NULL;
        char *dynamic_image_cq = NULL;

        snprintf(global_spots_15m_text, sizeof(global_spots_15m_text), "%d", snapshot.psk.global_spots_15m);
        snprintf(global_spots_60m_text, sizeof(global_spots_60m_text), "%d", snapshot.psk.global_spots_60m);
        snprintf(longest_path_text, sizeof(longest_path_text), "%d", snapshot.psk.longest_path_km);
        snprintf(best_snr_text, sizeof(best_snr_text), "%d", snapshot.psk.best_snr);
        snprintf(psk_score_text, sizeof(psk_score_text), "%d", snapshot.psk.score);
        snprintf(pskmap_spot_count_text, sizeof(pskmap_spot_count_text), "%d", spot_count);

        snprintf(sixm_alert_level_num_text, sizeof(sixm_alert_level_num_text), "%d",
            sixm_alert_num_from_snapshot(&snapshot, &settings));

        sb_init(&html);
        render_psk_snapshot_html(&html, &settings, &snapshot, spots, spot_count, station_lat, station_lon);
        detail[0] = '\0';
        if (app_capture_html_to_png(html.data ? html.data : "", "psk-snapshot", &png_data, &png_size, detail, sizeof(detail)) != 0 ||
            !png_data || png_size == 0) {
            const char *template_text = (settings.report_template_pskmap_failed[0] ? settings.report_template_pskmap_failed :
                "PSKReporter 6m 快照暂时生成失败：{{pskmap_error_detail}}\n台站：{{station_name}} ({{station_grid}})\n本地命中：15 分钟 {{psk_local_spots_15m}}，{{psk_window_minutes}} 分钟 {{psk_local_spots_60m}}\n最新：{{psk_latest_pair}}\n网页参考：{{pskmap_page_url}}");
            const char *error_detail = detail[0] ? detail : "未知错误";
            size_t render_cap = strlen(template_text) + strlen(error_detail) + strlen(page_url) + 4096;
            template_token_t tokens[] = {
                {"bot_name", settings.bot_name[0] ? settings.bot_name : APP_NAME},
                {"station_name", settings.station_name[0] ? settings.station_name : "-"},
                {"station_grid", settings.station_grid[0] ? settings.station_grid : "-"},
                {"psk_local_spots_15m", local_spots_15m_text},
                {"psk_local_spots_60m", local_spots_60m_text},
                {"psk_global_spots_15m", global_spots_15m_text},
                {"psk_global_spots_60m", global_spots_60m_text},
                {"psk_window_minutes", psk_window_text},
                {"psk_assessment", assessment},
                {"psk_assessment_code", assessment_code},
                {"psk_confidence", confidence},
                {"psk_confidence_code", confidence_code},
                {"psk_score", psk_score_text},
                {"psk_latest_pair", latest_pair},
                {"psk_latest_local_time", snapshot.psk.latest_local_time},
                {"psk_matched_grids", matched_grids},
                {"psk_farthest_peer", farthest_peer},
                {"psk_farthest_grid", farthest_grid},
                {"psk_longest_path_km", longest_path_text},
                {"psk_best_snr", best_snr_text},
                {"sixm_alert_level", alert_label},
                {"sixm_alert_level_num", sixm_alert_level_num_text},
                {"sixm_alert_code", alert_code},
                {"pskmap_spot_count", pskmap_spot_count_text},
                {"pskmap_error_detail", error_detail},
                {"pskmap_page_url", page_url},
                {"pskmap_image_cq", ""}
            };

            rendered_message = (char *)calloc(1, render_cap);
            if (rendered_message) {
                app_render_template(rendered_message, render_cap, template_text, tokens, sizeof(tokens) / sizeof(tokens[0]));
            }
            rc = onebot_send_message(app, type, target_id,
                rendered_message && rendered_message[0] ? rendered_message : "PSK 快照生成失败");
            app_log(app, rc == 0 ? "WARN" : "ERROR", "PSKReporter 快照图生成失败: target=%s detail=%s", target_id, error_detail);
            free(rendered_message);
            sb_free(&html);
            free(png_data);
            return rc;
        }

        base64 = base64_encode_alloc(png_data, png_size);
        free(png_data);
        png_data = NULL;
        sb_free(&html);
        if (!base64) {
            const char *template_text = (settings.report_template_pskmap_failed[0] ? settings.report_template_pskmap_failed :
                "PSKReporter 6m 快照暂时生成失败：{{pskmap_error_detail}}\n台站：{{station_name}} ({{station_grid}})\n本地命中：15 分钟 {{psk_local_spots_15m}}，{{psk_window_minutes}} 分钟 {{psk_local_spots_60m}}\n最新：{{psk_latest_pair}}\n网页参考：{{pskmap_page_url}}");
            const char *error_detail = "图片编码失败";
            size_t render_cap = strlen(template_text) + strlen(error_detail) + strlen(page_url) + 4096;
            template_token_t tokens[] = {
                {"bot_name", settings.bot_name[0] ? settings.bot_name : APP_NAME},
                {"station_name", settings.station_name[0] ? settings.station_name : "-"},
                {"station_grid", settings.station_grid[0] ? settings.station_grid : "-"},
                {"psk_local_spots_15m", local_spots_15m_text},
                {"psk_local_spots_60m", local_spots_60m_text},
                {"psk_global_spots_15m", global_spots_15m_text},
                {"psk_global_spots_60m", global_spots_60m_text},
                {"psk_window_minutes", psk_window_text},
                {"psk_assessment", assessment},
                {"psk_assessment_code", assessment_code},
                {"psk_confidence", confidence},
                {"psk_confidence_code", confidence_code},
                {"psk_score", psk_score_text},
                {"psk_latest_pair", latest_pair},
                {"psk_latest_local_time", snapshot.psk.latest_local_time},
                {"psk_matched_grids", matched_grids},
                {"psk_farthest_peer", farthest_peer},
                {"psk_farthest_grid", farthest_grid},
                {"psk_longest_path_km", longest_path_text},
                {"psk_best_snr", best_snr_text},
                {"sixm_alert_level", alert_label},
                {"sixm_alert_level_num", sixm_alert_level_num_text},
                {"sixm_alert_code", alert_code},
                {"pskmap_spot_count", pskmap_spot_count_text},
                {"pskmap_error_detail", error_detail},
                {"pskmap_page_url", page_url},
                {"pskmap_image_cq", ""}
            };

            rendered_message = (char *)calloc(1, render_cap);
            if (rendered_message) {
                app_render_template(rendered_message, render_cap, template_text, tokens, sizeof(tokens) / sizeof(tokens[0]));
            }
            rc = onebot_send_message(app, type, target_id,
                rendered_message && rendered_message[0] ? rendered_message : "PSK 快照生成失败");
            app_log(app, rc == 0 ? "WARN" : "ERROR", "PSKReporter 快照图 base64 编码失败: target=%s", target_id);
            free(rendered_message);
            return rc;
        }

        {
            const char *template_text = (settings.report_template_pskmap[0] ? settings.report_template_pskmap :
                "PSKReporter 6m 快照\n台站：{{station_name}} ({{station_grid}})\n本地命中：15 分钟 {{psk_local_spots_15m}}，{{psk_window_minutes}} 分钟 {{psk_local_spots_60m}}，判断：{{psk_assessment}}\n最新：{{psk_latest_pair}}\n{{pskmap_image_cq}}");
            size_t image_cq_cap = strlen(base64) + 32;
            size_t render_cap;
            template_token_t tokens[] = {
                {"bot_name", settings.bot_name[0] ? settings.bot_name : APP_NAME},
                {"station_name", settings.station_name[0] ? settings.station_name : "-"},
                {"station_grid", settings.station_grid[0] ? settings.station_grid : "-"},
                {"psk_local_spots_15m", local_spots_15m_text},
                {"psk_local_spots_60m", local_spots_60m_text},
                {"psk_global_spots_15m", global_spots_15m_text},
                {"psk_global_spots_60m", global_spots_60m_text},
                {"psk_window_minutes", psk_window_text},
                {"psk_assessment", assessment},
                {"psk_assessment_code", assessment_code},
                {"psk_confidence", confidence},
                {"psk_confidence_code", confidence_code},
                {"psk_score", psk_score_text},
                {"psk_latest_pair", latest_pair},
                {"psk_latest_local_time", snapshot.psk.latest_local_time},
                {"psk_matched_grids", matched_grids},
                {"psk_farthest_peer", farthest_peer},
                {"psk_farthest_grid", farthest_grid},
                {"psk_longest_path_km", longest_path_text},
                {"psk_best_snr", best_snr_text},
                {"sixm_alert_level", alert_label},
                {"sixm_alert_level_num", sixm_alert_level_num_text},
                {"sixm_alert_code", alert_code},
                {"pskmap_spot_count", pskmap_spot_count_text},
                {"pskmap_error_detail", ""},
                {"pskmap_page_url", page_url},
                {"pskmap_image_cq", ""}
            };

            dynamic_image_cq = (char *)calloc(1, image_cq_cap);
            if (dynamic_image_cq) {
                snprintf(dynamic_image_cq, image_cq_cap, "[CQ:image,file=base64://%s]", base64);
                tokens[sizeof(tokens) / sizeof(tokens[0]) - 1].value = dynamic_image_cq;
            }

            render_cap = strlen(template_text) + (dynamic_image_cq ? strlen(dynamic_image_cq) : 0) + strlen(page_url) + 4096;
            rendered_message = (char *)calloc(1, render_cap);
            if (rendered_message) {
                app_render_template(rendered_message, render_cap, template_text, tokens, sizeof(tokens) / sizeof(tokens[0]));
            }
        }

        rc = onebot_send_message(app, type, target_id,
            rendered_message && rendered_message[0] ? rendered_message : "PSK 快照");
        app_log(app, rc == 0 ? "INFO" : "ERROR", "PSKReporter 快照图发送%s: target=%s spots=%d",
            rc == 0 ? "成功" : "失败", target_id, spot_count);

        free(rendered_message);
        free(dynamic_image_cq);
        free(base64);
        return rc;
    }

    sb_init(&html);
    render_psk_snapshot_html(&html, &settings, &snapshot, spots, spot_count, station_lat, station_lon);
    detail[0] = '\0';
    if (app_capture_html_to_png(html.data ? html.data : "", "psk-snapshot", &png_data, &png_size, detail, sizeof(detail)) != 0 ||
        !png_data || png_size == 0) {
        sb_init(&message);
        sb_appendf(&message, "PSKReporter 6m 快照暂时生成失败：%s\n", detail[0] ? detail : "未知错误");
        sb_appendf(&message, "台站：%s (%s)\n",
            settings.station_name[0] ? settings.station_name : "-",
            settings.station_grid[0] ? settings.station_grid : "-");
        sb_appendf(&message, "本地命中：15 分钟 %d，%d 分钟 %d\n",
            snapshot.psk.local_spots_15m, settings.psk_window_minutes, snapshot.psk.local_spots_60m);
        sb_appendf(&message, "最新：%s\n", snapshot.psk.latest_pair[0] ? snapshot.psk.latest_pair : "暂无");
        sb_append(&message, "网页参考：https://pskreporter.info/pskmap.html");
        rc = onebot_send_message(app, type, target_id, message.data ? message.data : "PSK 快照生成失败");
        app_log(app, rc == 0 ? "WARN" : "ERROR", "PSKReporter 快照图生成失败: target=%s detail=%s", target_id, detail[0] ? detail : "unknown");
        sb_free(&message);
        sb_free(&html);
        free(png_data);
        return rc;
    }

    base64 = base64_encode_alloc(png_data, png_size);
    free(png_data);
    sb_free(&html);
    if (!base64) {
        app_log(app, "ERROR", "PSKReporter 快照图 base64 编码失败: target=%s", target_id);
        return -1;
    }

    sb_init(&message);
    sb_append(&message, "PSKReporter 6m 快照\n");
    sb_appendf(&message, "台站：%s (%s)\n",
        settings.station_name[0] ? settings.station_name : "-",
        settings.station_grid[0] ? settings.station_grid : "-");
    sb_appendf(&message, "本地命中：15 分钟 %d，%d 分钟 %d；判断：%s\n",
        snapshot.psk.local_spots_15m,
        settings.psk_window_minutes,
        snapshot.psk.local_spots_60m,
        snapshot.psk.assessment[0] ? snapshot.psk.assessment : "等待数据");
    sb_appendf(&message, "最新：%s\n", snapshot.psk.latest_pair[0] ? snapshot.psk.latest_pair : "暂无");
    sb_append(&message, "[CQ:image,file=base64://");
    sb_append(&message, base64);
    sb_append(&message, "]");

    rc = onebot_send_message(app, type, target_id, message.data ? message.data : "");
    app_log(app, rc == 0 ? "INFO" : "ERROR", "PSKReporter 快照图发送%s: target=%s spots=%d",
        rc == 0 ? "成功" : "失败", target_id, spot_count);

    free(base64);
    sb_free(&message);
    return rc;
}

static int prepare_psk_snapshot_payload(app_t *app, const char *band,
                                        settings_t *settings, snapshot_t *snapshot,
                                        psk_map_spot_t *spots, int max_spots, int *spot_count,
                                        double *station_lat, double *station_lon) {
    if (!app || !settings || !snapshot || !spots || !spot_count || !station_lat || !station_lon) {
        return -1;
    }
    app_rebuild_snapshot(app);
    pthread_mutex_lock(&app->cache_mutex);
    *settings = app->settings;
    *snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);
    *spot_count = collect_recent_map_spots(app, settings, normalize_psk_band(band),
        spots, max_spots, station_lat, station_lon);
    return 0;
}

static char *render_pskmap_message_alloc(const settings_t *settings, const snapshot_t *snapshot, const char *band,
                                         int spot_count, const char *template_text,
                                         const char *error_detail, const char *image_cq) {
    const psk_summary_t *summary = summary_for_band(snapshot, band);
    const char *band_label = label_for_band(band);
    const char *band_freq = mhz_label_for_band(band);
    const char *assessment = summary->assessment[0] ? summary->assessment :
        (settings->wording_psk_assessment_disconnected[0] ? settings->wording_psk_assessment_disconnected : "实时数据未连接");
    const char *confidence = summary->confidence[0] ? summary->confidence :
        (settings->wording_psk_confidence_unknown[0] ? settings->wording_psk_confidence_unknown : "未知");
    const char *latest_pair = summary->latest_pair[0] ? summary->latest_pair : "暂无";
    const char *matched_grids = summary->matched_grids[0] ? summary->matched_grids : "暂无";
    const char *farthest_peer = summary->farthest_peer[0] ? summary->farthest_peer : "暂无";
    const char *farthest_grid = summary->farthest_grid[0] ? summary->farthest_grid : "暂无";
    const char *assessment_code = psk_assessment_code_from_summary(summary);
    const char *confidence_code = psk_confidence_code_from_summary(summary);
    const char *alert_label = alert_label_for_band(snapshot, settings, band);
    const char *alert_code = alert_code_for_band(snapshot, settings, band);
    char local_spots_15m_text[32];
    char local_spots_60m_text[32];
    char global_spots_15m_text[32];
    char global_spots_60m_text[32];
    char longest_path_text[32];
    char best_snr_text[32];
    char psk_score_text[32];
    char psk_window_text[32];
    char pskmap_spot_count_text[32];
    char band_alert_level_num_text[32];
    char page_url[MAX_LARGE_TEXT];
    size_t render_cap;
    char *rendered;
    template_token_t tokens[] = {
        {"bot_name", settings->bot_name[0] ? settings->bot_name : APP_NAME},
        {"station_name", settings->station_name[0] ? settings->station_name : "-"},
        {"station_grid", settings->station_grid[0] ? settings->station_grid : "-"},
        {"pskmap_band", band_label},
        {"pskmap_frequency", band_freq},
        {"psk_local_spots_15m", local_spots_15m_text},
        {"psk_local_spots_60m", local_spots_60m_text},
        {"psk_global_spots_15m", global_spots_15m_text},
        {"psk_global_spots_60m", global_spots_60m_text},
        {"psk_window_minutes", psk_window_text},
        {"psk_assessment", assessment},
        {"psk_assessment_code", assessment_code},
        {"psk_confidence", confidence},
        {"psk_confidence_code", confidence_code},
        {"psk_score", psk_score_text},
        {"psk_latest_pair", latest_pair},
        {"psk_latest_local_time", summary->latest_local_time},
        {"psk_matched_grids", matched_grids},
        {"psk_farthest_peer", farthest_peer},
        {"psk_farthest_grid", farthest_grid},
        {"psk_longest_path_km", longest_path_text},
        {"psk_best_snr", best_snr_text},
        {"band_alert_level", alert_label},
        {"band_alert_level_num", band_alert_level_num_text},
        {"band_alert_code", alert_code},
        {"sixm_alert_level", alert_label},
        {"sixm_alert_level_num", band_alert_level_num_text},
        {"sixm_alert_code", alert_code},
        {"twom_alert_level", alert_label},
        {"twom_alert_level_num", band_alert_level_num_text},
        {"twom_alert_code", alert_code},
        {"pskmap_spot_count", pskmap_spot_count_text},
        {"pskmap_error_detail", error_detail ? error_detail : ""},
        {"pskmap_page_url", page_url},
        {"pskmap_image_cq", image_cq ? image_cq : ""}
    };

    snprintf(local_spots_15m_text, sizeof(local_spots_15m_text), "%d", summary->local_spots_15m);
    snprintf(local_spots_60m_text, sizeof(local_spots_60m_text), "%d", summary->local_spots_60m);
    snprintf(global_spots_15m_text, sizeof(global_spots_15m_text), "%d", summary->global_spots_15m);
    snprintf(global_spots_60m_text, sizeof(global_spots_60m_text), "%d", summary->global_spots_60m);
    snprintf(longest_path_text, sizeof(longest_path_text), "%d", summary->longest_path_km);
    snprintf(best_snr_text, sizeof(best_snr_text), "%d", summary->best_snr > -999 ? summary->best_snr : 0);
    snprintf(psk_score_text, sizeof(psk_score_text), "%d", summary->score);
    snprintf(psk_window_text, sizeof(psk_window_text), "%d", settings->psk_window_minutes);
    snprintf(pskmap_spot_count_text, sizeof(pskmap_spot_count_text), "%d", spot_count);
    snprintf(band_alert_level_num_text, sizeof(band_alert_level_num_text), "%d",
        alert_num_for_band(snapshot, settings, band));
    copy_string(page_url, sizeof(page_url), "https://pskreporter.info/pskmap.html");

    render_cap = strlen(template_text) + strlen(settings->station_name) + strlen(settings->station_grid) +
        strlen(latest_pair) + strlen(matched_grids) + strlen(farthest_peer) + strlen(farthest_grid) +
        strlen(alert_label) + strlen(page_url) + (error_detail ? strlen(error_detail) : 0) +
        (image_cq ? strlen(image_cq) : 0) + 8192;
    rendered = (char *)calloc(1, render_cap);
    if (!rendered) {
        return NULL;
    }
    app_render_template(rendered, render_cap, template_text, tokens, sizeof(tokens) / sizeof(tokens[0]));
    return rendered;
}

#ifdef _WIN32
static ARGB map_argb(BYTE a, BYTE r, BYTE g, BYTE b) {
    return ((ARGB)a << 24) | ((ARGB)r << 16) | ((ARGB)g << 8) | (ARGB)b;
}

static WCHAR *utf8_to_wide_dup_local(const char *text) {
    int wide_len;
    WCHAR *wide;
    if (!text) {
        text = "";
    }
    wide_len = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (wide_len <= 0) {
        return NULL;
    }
    wide = (WCHAR *)calloc((size_t)wide_len, sizeof(WCHAR));
    if (!wide) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, wide_len) <= 0) {
        free(wide);
        return NULL;
    }
    return wide;
}

static GpFont *create_font_local(REAL size, INT style) {
    static const WCHAR *preferred[] = {L"Microsoft YaHei UI", L"Microsoft YaHei", L"Segoe UI"};
    GpFontFamily *family = NULL;
    GpFont *font = NULL;
    for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); ++i) {
        if (GdipCreateFontFamilyFromName(preferred[i], NULL, &family) == Ok && family) {
            break;
        }
        family = NULL;
    }
    if (!family) {
        GdipGetGenericFontFamilySansSerif(&family);
    }
    if (!family) {
        return NULL;
    }
    if (GdipCreateFont(family, size, style, UnitPixel, &font) != Ok) {
        font = NULL;
    }
    GdipDeleteFontFamily(family);
    return font;
}

static void fill_rect_local(GpGraphics *graphics, INT x, INT y, INT w, INT h, ARGB color) {
    GpSolidFill *brush = NULL;
    if (!graphics || GdipCreateSolidFill(color, &brush) != Ok || !brush) {
        return;
    }
    GdipFillRectangleI(graphics, (GpBrush *)brush, x, y, w, h);
    GdipDeleteBrush((GpBrush *)brush);
}

static void draw_rect_local(GpGraphics *graphics, INT x, INT y, INT w, INT h, ARGB color, REAL width) {
    GpPen *pen = NULL;
    if (!graphics || GdipCreatePen1(color, width, UnitPixel, &pen) != Ok || !pen) {
        return;
    }
    GdipDrawRectangleI(graphics, pen, x, y, w, h);
    GdipDeletePen(pen);
}

static void draw_line_local(GpGraphics *graphics, INT x1, INT y1, INT x2, INT y2,
                            ARGB color, REAL width, int rounded) {
    GpPen *pen = NULL;
    if (!graphics || GdipCreatePen1(color, width, UnitPixel, &pen) != Ok || !pen) {
        return;
    }
    if (rounded) {
        GdipSetPenStartCap(pen, LineCapRound);
        GdipSetPenEndCap(pen, LineCapRound);
        GdipSetPenLineJoin(pen, LineJoinRound);
    }
    GdipDrawLineI(graphics, pen, x1, y1, x2, y2);
    GdipDeletePen(pen);
}

static void fill_circle_local(GpGraphics *graphics, INT cx, INT cy, INT radius, ARGB color) {
    GpSolidFill *brush = NULL;
    if (!graphics || GdipCreateSolidFill(color, &brush) != Ok || !brush) {
        return;
    }
    GdipFillEllipseI(graphics, (GpBrush *)brush, cx - radius, cy - radius, radius * 2, radius * 2);
    GdipDeleteBrush((GpBrush *)brush);
}

static void draw_circle_local(GpGraphics *graphics, INT cx, INT cy, INT radius, ARGB color, REAL width) {
    GpPen *pen = NULL;
    if (!graphics || GdipCreatePen1(color, width, UnitPixel, &pen) != Ok || !pen) {
        return;
    }
    GdipDrawEllipseI(graphics, pen, cx - radius, cy - radius, radius * 2, radius * 2);
    GdipDeletePen(pen);
}

static void draw_text_local(GpGraphics *graphics, const char *text,
                            REAL x, REAL y, REAL w, REAL h,
                            GpFont *font, ARGB color, StringAlignment align) {
    WCHAR *wide;
    GpSolidFill *brush = NULL;
    GpStringFormat *format = NULL;
    RectF rect;
    if (!graphics || !font || !text || !*text) {
        return;
    }
    wide = utf8_to_wide_dup_local(text);
    if (!wide) {
        return;
    }
    if (GdipCreateSolidFill(color, &brush) != Ok || !brush) {
        free(wide);
        return;
    }
    if (GdipCreateStringFormat(0, 0, &format) == Ok && format) {
        GdipSetStringFormatAlign(format, align);
        GdipSetStringFormatLineAlign(format, StringAlignmentNear);
    }
    rect.X = x;
    rect.Y = y;
    rect.Width = w;
    rect.Height = h;
    GdipDrawString(graphics, wide, -1, font, &rect, format, (GpBrush *)brush);
    if (format) {
        GdipDeleteStringFormat(format);
    }
    GdipDeleteBrush((GpBrush *)brush);
    free(wide);
}

static void draw_stat_card_local(GpGraphics *graphics, INT x, INT y, INT w, INT h,
                                 GpFont *label_font, GpFont *value_font,
                                 const char *label, const char *value) {
    fill_rect_local(graphics, x, y, w, h, map_argb(230, 255, 252, 247));
    draw_rect_local(graphics, x, y, w, h, map_argb(255, 224, 209, 191), 1.0f);
    draw_text_local(graphics, label, (REAL)x + 16.0f, (REAL)y + 12.0f, (REAL)w - 32.0f, 20.0f,
        label_font, map_argb(255, 140, 111, 86), StringAlignmentNear);
    draw_text_local(graphics, value, (REAL)x + 16.0f, (REAL)y + 38.0f, (REAL)w - 32.0f, 34.0f,
        value_font, map_argb(255, 46, 33, 24), StringAlignmentNear);
}

static int find_png_encoder_clsid_local(CLSID *clsid) {
    UINT count = 0;
    UINT bytes = 0;
    ImageCodecInfo *codecs = NULL;
    if (!clsid || GetImageEncodersSize(&count, &bytes) != Ok || count == 0 || bytes == 0) {
        return -1;
    }
    codecs = (ImageCodecInfo *)malloc(bytes);
    if (!codecs) {
        return -1;
    }
    if (GetImageEncoders(count, bytes, codecs) != Ok) {
        free(codecs);
        return -1;
    }
    for (UINT i = 0; i < count; ++i) {
        if (codecs[i].MimeType && wcscmp(codecs[i].MimeType, L"image/png") == 0) {
            *clsid = codecs[i].Clsid;
            free(codecs);
            return 0;
        }
    }
    free(codecs);
    return -1;
}

static int render_psk_snapshot_png_windows(const settings_t *settings, const snapshot_t *snapshot, const char *band,
                                           const psk_map_spot_t *spots, int spot_count,
                                           double station_lat, double station_lon,
                                           unsigned char **png_data, size_t *png_size,
                                           char *detail, size_t detail_len) {
    const char *band_label = label_for_band(band);
    const char *freq_label = mhz_label_for_band(band);
    const psk_summary_t *summary = summary_for_band(snapshot, band);
    const char *alert_label = alert_label_for_band(snapshot, settings, band);
    const INT canvas_w = 1360;
    const INT canvas_h = 940;
    const INT margin = 34;
    const INT stats_y = 118;
    const INT stat_w = 246;
    const INT stat_h = 92;
    const INT stat_gap = 12;
    const INT map_x = 34;
    const INT map_y = 234;
    const INT map_w = 862;
    const INT map_h = 600;
    const INT plot_x = map_x + 44;
    const INT plot_y = map_y + 72;
    const INT plot_w = 776;
    const INT plot_h = 438;
    const INT side_x = 918;
    const INT side_y = 234;
    const INT side_w = 408;
    const INT side_h = 600;
    const INT footer_y = 858;
    GdiplusStartupInput startup_input;
    ULONG_PTR token = 0;
    GpBitmap *bitmap = NULL;
    GpGraphics *graphics = NULL;
    GpLineGradient *bg_brush = NULL;
    GpFont *font_title = NULL;
    GpFont *font_subtitle = NULL;
    GpFont *font_card_label = NULL;
    GpFont *font_card_value = NULL;
    GpFont *font_panel_title = NULL;
    GpFont *font_body = NULL;
    GpFont *font_small = NULL;
    psk_map_bounds_t bounds;
    char generated_at[MAX_TEXT];
    char stat_text[64];
    char summary_text[1024];
    char latest_text[512];
    char title_text[128];
    char subtitle_text[512];
    CLSID png_clsid;
    IStream *stream = NULL;
    HGLOBAL hglobal = NULL;
    void *stream_ptr = NULL;
    SIZE_T stream_size = 0;
    HRESULT hr;

    if (!png_data || !png_size) {
        return -1;
    }
    *png_data = NULL;
    *png_size = 0;
    if (detail && detail_len > 0) {
        detail[0] = '\0';
    }

    hr = CoInitialize(NULL);
    startup_input.GdiplusVersion = 1;
    startup_input.DebugEventCallback = NULL;
    startup_input.SuppressBackgroundThread = FALSE;
    startup_input.SuppressExternalCodecs = FALSE;
    if (GdiplusStartup(&token, &startup_input, NULL) != Ok) {
        copy_string(detail, detail_len, "GDI+ 启动失败");
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
        return -1;
    }
    if (GdipCreateBitmapFromScan0(canvas_w, canvas_h, 0, PixelFormat32bppARGB, NULL, &bitmap) != Ok || !bitmap ||
        GdipGetImageGraphicsContext((GpImage *)bitmap, &graphics) != Ok || !graphics) {
        copy_string(detail, detail_len, "无法创建画布");
        goto cleanup;
    }

    GdipSetPageUnit(graphics, UnitPixel);
    GdipSetSmoothingMode(graphics, SmoothingModeAntiAlias);
    GdipSetTextRenderingHint(graphics, TextRenderingHintClearTypeGridFit);
    GdipSetInterpolationMode(graphics, InterpolationModeHighQualityBicubic);
    GdipSetCompositingQuality(graphics, CompositingQualityHighQuality);

    {
        Rect rect;
        rect.X = 0;
        rect.Y = 0;
        rect.Width = canvas_w;
        rect.Height = canvas_h;
        if (GdipCreateLineBrushFromRectI(&rect, map_argb(255, 255, 249, 239), map_argb(255, 233, 218, 193),
                LinearGradientModeVertical, WrapModeClamp, &bg_brush) == Ok && bg_brush) {
            GdipFillRectangleI(graphics, (GpBrush *)bg_brush, 0, 0, canvas_w, canvas_h);
        } else {
            GdipGraphicsClear(graphics, map_argb(255, 248, 239, 226));
        }
    }

    font_title = create_font_local(30.0f, FontStyleBold);
    font_subtitle = create_font_local(14.0f, FontStyleRegular);
    font_card_label = create_font_local(13.0f, FontStyleRegular);
    font_card_value = create_font_local(24.0f, FontStyleBold);
    font_panel_title = create_font_local(19.0f, FontStyleBold);
    font_body = create_font_local(13.5f, FontStyleRegular);
    font_small = create_font_local(11.5f, FontStyleRegular);
    if (!font_title || !font_subtitle || !font_card_label || !font_card_value || !font_panel_title || !font_body || !font_small) {
        copy_string(detail, detail_len, "无法创建字体");
        goto cleanup;
    }

    format_time_local(time(NULL), generated_at, sizeof(generated_at));
    snprintf(title_text, sizeof(title_text), "PSKReporter %s 本地传播图", band_label);
    snprintf(subtitle_text, sizeof(subtitle_text), "%s | 台站 %s | 网格 %s | 监控 %s | 生成于 %s",
        freq_label,
        settings->station_name[0] ? settings->station_name : "-",
        settings->station_grid[0] ? settings->station_grid : "-",
        settings->psk_grids[0] ? settings->psk_grids : settings->station_grid,
        generated_at);
    draw_text_local(graphics, title_text, (REAL)margin, 28.0f, 860.0f, 42.0f,
        font_title, map_argb(255, 34, 24, 18), StringAlignmentNear);
    draw_text_local(graphics, subtitle_text, (REAL)margin, 72.0f, 980.0f, 24.0f,
        font_subtitle, map_argb(255, 108, 91, 76), StringAlignmentNear);

    fill_rect_local(graphics, 1178, 34, 148, 44, map_argb(255, 27, 119, 110));
    draw_text_local(graphics, summary->assessment[0] ? summary->assessment : "等待数据",
        1188.0f, 46.0f, 128.0f, 22.0f, font_small, map_argb(255, 255, 255, 255), StringAlignmentCenter);

    snprintf(stat_text, sizeof(stat_text), "%d", summary->local_spots_15m);
    draw_stat_card_local(graphics, margin, stats_y, stat_w, stat_h, font_card_label, font_card_value, "15 分钟本地命中", stat_text);
    snprintf(stat_text, sizeof(stat_text), "%d", summary->local_spots_60m);
    draw_stat_card_local(graphics, margin + (stat_w + stat_gap), stats_y, stat_w, stat_h, font_card_label, font_card_value, "窗口内本地命中", stat_text);
    snprintf(stat_text, sizeof(stat_text), "%d km", summary->longest_path_km);
    draw_stat_card_local(graphics, margin + 2 * (stat_w + stat_gap), stats_y, stat_w, stat_h, font_card_label, font_card_value, "最远路径", stat_text);
    snprintf(stat_text, sizeof(stat_text), "%d dB", summary->best_snr > -999 ? summary->best_snr : 0);
    draw_stat_card_local(graphics, margin + 3 * (stat_w + stat_gap), stats_y, stat_w, stat_h, font_card_label, font_card_value, "最佳 SNR", stat_text);
    draw_stat_card_local(graphics, margin + 4 * (stat_w + stat_gap), stats_y, stat_w, stat_h, font_card_label, font_card_value, "综合提醒", alert_label);

    fill_rect_local(graphics, map_x, map_y, map_w, map_h, map_argb(228, 255, 252, 248));
    draw_rect_local(graphics, map_x, map_y, map_w, map_h, map_argb(255, 221, 205, 186), 1.0f);
    draw_text_local(graphics, "路径地图", (REAL)map_x + 18.0f, (REAL)map_y + 18.0f, 280.0f, 24.0f,
        font_panel_title, map_argb(255, 40, 28, 21), StringAlignmentNear);
    draw_text_local(graphics, "颜色越暖表示越新，线越粗表示 SNR 越强。", (REAL)map_x + 18.0f, (REAL)map_y + 46.0f, 420.0f, 20.0f,
        font_small, map_argb(255, 115, 94, 77), StringAlignmentNear);

    memset(&bounds, 0, sizeof(bounds));
    bounds_include(&bounds, station_lat, station_lon);
    for (int i = 0; i < spot_count; ++i) {
        if (spots[i].has_peer_coords) {
            bounds_include(&bounds, spots[i].peer_lat, spots[i].peer_lon);
        }
    }
    finalize_bounds(&bounds, station_lat, station_lon);

    fill_rect_local(graphics, plot_x, plot_y, plot_w, plot_h, map_argb(255, 255, 253, 249));
    draw_rect_local(graphics, plot_x, plot_y, plot_w, plot_h, map_argb(255, 222, 209, 192), 1.0f);
    {
        double lon_step = axis_step(bounds.max_lon - bounds.min_lon);
        double lat_step = axis_step(bounds.max_lat - bounds.min_lat);
        double lon_value = floor(bounds.min_lon / lon_step) * lon_step;
        double lat_value = floor(bounds.min_lat / lat_step) * lat_step;
        for (; lon_value <= bounds.max_lon + 0.001; lon_value += lon_step) {
            INT x = (INT)llround(lon_to_x(lon_value, &bounds, plot_x, plot_w));
            char axis_label[32];
            format_axis_label(lon_value, 1, axis_label, sizeof(axis_label));
            draw_line_local(graphics, x, plot_y, x, plot_y + plot_h, map_argb(255, 236, 223, 208), 1.0f, 0);
            draw_text_local(graphics, axis_label, (REAL)x - 34.0f, (REAL)(plot_y + plot_h + 8), 68.0f, 16.0f,
                font_small, map_argb(255, 142, 118, 94), StringAlignmentCenter);
        }
        for (; lat_value <= bounds.max_lat + 0.001; lat_value += lat_step) {
            INT y = (INT)llround(lat_to_y(lat_value, &bounds, plot_y, plot_h));
            char axis_label[32];
            format_axis_label(lat_value, 0, axis_label, sizeof(axis_label));
            draw_line_local(graphics, plot_x, y, plot_x + plot_w, y, map_argb(255, 236, 223, 208), 1.0f, 0);
            draw_text_local(graphics, axis_label, (REAL)(plot_x - 56), (REAL)y - 8.0f, 46.0f, 16.0f,
                font_small, map_argb(255, 142, 118, 94), StringAlignmentCenter);
        }
    }

    {
        INT sx = (INT)llround(lon_to_x(station_lon, &bounds, plot_x, plot_w));
        INT sy = (INT)llround(lat_to_y(station_lat, &bounds, plot_y, plot_h));
        GdipSetClipRectI(graphics, plot_x, plot_y, plot_w, plot_h, CombineModeReplace);
        for (int i = spot_count - 1; i >= 0; --i) {
            if (!spots[i].has_peer_coords) {
                continue;
            }
            INT px = (INT)llround(lon_to_x(spots[i].peer_lon, &bounds, plot_x, plot_w));
            INT py = (INT)llround(lat_to_y(spots[i].peer_lat, &bounds, plot_y, plot_h));
            double age_minutes = difftime(time(NULL), spots[i].timestamp) / 60.0;
            double age_ratio = clamp_double(age_minutes / (double)clamp_int(settings->psk_window_minutes, 15, 720), 0.0, 1.0);
            BYTE red = (BYTE)(233 - (int)(age_ratio * 96.0));
            BYTE green = (BYTE)(118 + (int)(age_ratio * 52.0));
            BYTE blue = (BYTE)(71 + (int)(age_ratio * 104.0));
            REAL line_width = (REAL)(1.4 + clamp_double((spots[i].snr + 20) / 18.0, 0.0, 3.4));
            draw_line_local(graphics, sx, sy, px, py, map_argb(188, red, green, blue), line_width, 1);
            fill_circle_local(graphics, px, py, 6, map_argb(242, red, green, blue));
            draw_circle_local(graphics, px, py, 6, map_argb(255, 255, 250, 245), 1.3f);
        }
        GdipResetClip(graphics);
        fill_circle_local(graphics, sx, sy, 17, map_argb(42, 196, 70, 44));
        fill_circle_local(graphics, sx, sy, 8, map_argb(255, 196, 70, 44));
        draw_circle_local(graphics, sx, sy, 8, map_argb(255, 255, 247, 240), 2.0f);
        draw_text_local(graphics, "本站", (REAL)sx + 12.0f, (REAL)sy - 16.0f, 60.0f, 18.0f,
            font_small, map_argb(255, 98, 60, 44), StringAlignmentNear);
    }

    fill_rect_local(graphics, side_x, side_y, side_w, side_h, map_argb(236, 255, 253, 250));
    draw_rect_local(graphics, side_x, side_y, side_w, side_h, map_argb(255, 221, 205, 186), 1.0f);
    draw_text_local(graphics, "最近命中", (REAL)side_x + 18.0f, (REAL)side_y + 18.0f, 180.0f, 24.0f,
        font_panel_title, map_argb(255, 40, 28, 21), StringAlignmentNear);
    snprintf(summary_text, sizeof(summary_text),
        "判断：%s\n置信度：%s\n评分：%d / 100\n命中网格：%s\n最远路径：%s %s 约 %d km",
        summary->assessment[0] ? summary->assessment : "等待数据",
        summary->confidence[0] ? summary->confidence : "未知",
        summary->score,
        summary->matched_grids[0] ? summary->matched_grids : "暂无",
        summary->farthest_peer[0] ? summary->farthest_peer : "暂无",
        summary->farthest_grid[0] ? summary->farthest_grid : "",
        summary->longest_path_km);
    draw_text_local(graphics, summary_text, (REAL)side_x + 18.0f, (REAL)side_y + 54.0f, (REAL)side_w - 36.0f, 112.0f,
        font_body, map_argb(255, 78, 64, 52), StringAlignmentNear);

    draw_line_local(graphics, side_x + 18, side_y + 180, side_x + side_w - 18, side_y + 180, map_argb(255, 232, 221, 209), 1.0f, 0);
    draw_text_local(graphics, "时间", (REAL)side_x + 18.0f, (REAL)side_y + 192.0f, 72.0f, 18.0f,
        font_small, map_argb(255, 142, 118, 94), StringAlignmentNear);
    draw_text_local(graphics, "路径 / 模式", (REAL)side_x + 104.0f, (REAL)side_y + 192.0f, 190.0f, 18.0f,
        font_small, map_argb(255, 142, 118, 94), StringAlignmentNear);
    draw_text_local(graphics, "SNR", (REAL)side_x + 300.0f, (REAL)side_y + 192.0f, 40.0f, 18.0f,
        font_small, map_argb(255, 142, 118, 94), StringAlignmentNear);
    draw_text_local(graphics, "距离", (REAL)side_x + 350.0f, (REAL)side_y + 192.0f, 42.0f, 18.0f,
        font_small, map_argb(255, 142, 118, 94), StringAlignmentNear);

    if (spot_count == 0) {
        draw_text_local(graphics, "最近窗口内还没有与本地网格相关的 spot。",
            (REAL)side_x + 18.0f, (REAL)side_y + 228.0f, (REAL)side_w - 36.0f, 24.0f,
            font_body, map_argb(255, 110, 92, 78), StringAlignmentNear);
    } else {
        INT row_y = side_y + 220;
        for (int i = 0; i < spot_count && i < 8; ++i) {
            char ts[MAX_TEXT];
            char path_text[256];
            char snr_text[32];
            char dist_text[32];
            format_time_local(spots[i].timestamp, ts, sizeof(ts));
            snprintf(path_text, sizeof(path_text), "%s %s -> %s %s\n%s",
                spots[i].sender_call, spots[i].sender_grid,
                spots[i].receiver_call, spots[i].receiver_grid,
                spots[i].mode[0] ? spots[i].mode : "-");
            snprintf(snr_text, sizeof(snr_text), "%d", spots[i].snr);
            snprintf(dist_text, sizeof(dist_text), "%d km", spots[i].distance_km);
            draw_line_local(graphics, side_x + 18, row_y - 8, side_x + side_w - 18, row_y - 8, map_argb(255, 239, 228, 216), 1.0f, 0);
            draw_text_local(graphics, ts, (REAL)side_x + 18.0f, (REAL)row_y, 76.0f, 30.0f,
                font_small, map_argb(255, 90, 72, 59), StringAlignmentNear);
            draw_text_local(graphics, path_text, (REAL)side_x + 104.0f, (REAL)row_y, 186.0f, 36.0f,
                font_small, map_argb(255, 60, 46, 35), StringAlignmentNear);
            draw_text_local(graphics, snr_text, (REAL)side_x + 300.0f, (REAL)row_y, 40.0f, 20.0f,
                font_small, map_argb(255, 60, 46, 35), StringAlignmentNear);
            draw_text_local(graphics, dist_text, (REAL)side_x + 346.0f, (REAL)row_y, 54.0f, 20.0f,
                font_small, map_argb(255, 60, 46, 35), StringAlignmentNear);
            row_y += 46;
        }
    }

    snprintf(latest_text, sizeof(latest_text), "最新链路：%s",
        summary->latest_pair[0] ? summary->latest_pair : "暂无");
    draw_text_local(graphics, latest_text, (REAL)margin, (REAL)footer_y, 1020.0f, 22.0f,
        font_body, map_argb(255, 84, 66, 52), StringAlignmentNear);
    draw_text_local(graphics, "本图由程序依据本地 PSKReporter MQTT 数据直接生成，不再依赖网页截图。",
        (REAL)margin, (REAL)(footer_y + 26), 1080.0f, 18.0f,
        font_small, map_argb(255, 115, 94, 77), StringAlignmentNear);

    if (find_png_encoder_clsid_local(&png_clsid) != 0) {
        copy_string(detail, detail_len, "无法找到 PNG 编码器");
        goto cleanup;
    }
    if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || !stream) {
        copy_string(detail, detail_len, "无法创建内存流");
        goto cleanup;
    }
    if (GdipSaveImageToStream((GpImage *)bitmap, stream, &png_clsid, NULL) != Ok) {
        copy_string(detail, detail_len, "PNG 输出失败");
        goto cleanup;
    }
    if (GetHGlobalFromStream(stream, &hglobal) != S_OK || !hglobal) {
        copy_string(detail, detail_len, "无法读取 PNG 内存流");
        goto cleanup;
    }
    stream_size = GlobalSize(hglobal);
    stream_ptr = GlobalLock(hglobal);
    if (!stream_ptr || stream_size == 0) {
        copy_string(detail, detail_len, "PNG 内存流为空");
        goto cleanup;
    }
    *png_data = (unsigned char *)malloc((size_t)stream_size);
    if (!*png_data) {
        copy_string(detail, detail_len, "PNG 内存分配失败");
        goto cleanup;
    }
    memcpy(*png_data, stream_ptr, (size_t)stream_size);
    *png_size = (size_t)stream_size;

cleanup:
    if (stream_ptr) {
        GlobalUnlock(hglobal);
    }
    if (stream) {
        stream->lpVtbl->Release(stream);
    }
    if (bg_brush) {
        GdipDeleteBrush((GpBrush *)bg_brush);
    }
    if (font_title) GdipDeleteFont(font_title);
    if (font_subtitle) GdipDeleteFont(font_subtitle);
    if (font_card_label) GdipDeleteFont(font_card_label);
    if (font_card_value) GdipDeleteFont(font_card_value);
    if (font_panel_title) GdipDeleteFont(font_panel_title);
    if (font_body) GdipDeleteFont(font_body);
    if (font_small) GdipDeleteFont(font_small);
    if (graphics) {
        GdipDeleteGraphics(graphics);
    }
    if (bitmap) {
        GdipDisposeImage((GpImage *)bitmap);
    }
    if (token) {
        GdiplusShutdown(token);
    }
    if (SUCCEEDED(hr)) {
        CoUninitialize();
    }
    if (!*png_data && detail && !detail[0]) {
        copy_string(detail, detail_len, "未知绘图错误");
    }
    return *png_data ? 0 : -1;
}
#endif

static int render_psk_snapshot_png_common(const settings_t *settings, const snapshot_t *snapshot, const char *band,
                                          const psk_map_spot_t *spots, int spot_count,
                                          double station_lat, double station_lon,
                                          unsigned char **png_data, size_t *png_size,
                                          char *detail, size_t detail_len) {
#ifdef _WIN32
    if (render_psk_snapshot_png_windows(settings, snapshot, band, spots, spot_count,
            station_lat, station_lon, png_data, png_size, detail, detail_len) == 0) {
        return 0;
    }
#endif
    copy_string(detail, detail_len, "当前环境未启用本地图形渲染");
    return -1;
}

int psk_render_snapshot_png_for_band(app_t *app, const char *band, unsigned char **png_data, size_t *png_size,
                                     char *detail, size_t detail_len) {
    settings_t settings;
    snapshot_t snapshot;
    psk_map_spot_t spots[12];
    int spot_count = 0;
    double station_lat = 0.0;
    double station_lon = 0.0;
    if (prepare_psk_snapshot_payload(app, normalize_psk_band(band), &settings, &snapshot,
            spots, (int)(sizeof(spots) / sizeof(spots[0])), &spot_count, &station_lat, &station_lon) != 0) {
        copy_string(detail, detail_len, "无法准备地图数据");
        return -1;
    }
    return render_psk_snapshot_png_common(&settings, &snapshot, normalize_psk_band(band),
        spots, spot_count, station_lat, station_lon, png_data, png_size, detail, detail_len);
}

int psk_send_snapshot_image_for_band(app_t *app, target_type_t type, const char *target_id, const char *band) {
    settings_t settings;
    snapshot_t snapshot;
    psk_map_spot_t spots[12];
    int spot_count = 0;
    double station_lat = 0.0;
    double station_lon = 0.0;
    unsigned char *png_data = NULL;
    size_t png_size = 0;
    char detail[256];
    char *base64 = NULL;
    char *image_cq = NULL;
    char *message = NULL;
    int rc = -1;
    const char *normalized_band = normalize_psk_band(band);
    const char *success_template =
        "PSKReporter {{pskmap_band}} 快照\n台站：{{station_name}} ({{station_grid}})\n本地命中：15 分钟 {{psk_local_spots_15m}}，{{psk_window_minutes}} 分钟 {{psk_local_spots_60m}}，判断：{{psk_assessment}}\n最新：{{psk_latest_pair}}\n{{pskmap_image_cq}}";
    const char *failed_template =
        "PSKReporter {{pskmap_band}} 快照暂时生成失败：{{pskmap_error_detail}}\n台站：{{station_name}} ({{station_grid}})\n本地命中：15 分钟 {{psk_local_spots_15m}}，{{psk_window_minutes}} 分钟 {{psk_local_spots_60m}}\n最新：{{psk_latest_pair}}\n网页参考：{{pskmap_page_url}}";
    const char *configured_success_template = success_template;
    const char *configured_failed_template = failed_template;

    if (!app || !target_id || !*target_id) {
        return -1;
    }

    detail[0] = '\0';
    if (prepare_psk_snapshot_payload(app, normalized_band, &settings, &snapshot,
            spots, (int)(sizeof(spots) / sizeof(spots[0])), &spot_count, &station_lat, &station_lon) != 0) {
        copy_string(detail, sizeof(detail), "无法准备地图数据");
    } else if (render_psk_snapshot_png_common(&settings, &snapshot, normalized_band,
            spots, spot_count, station_lat, station_lon, &png_data, &png_size, detail, sizeof(detail)) == 0 &&
        png_data && png_size > 0) {
        base64 = base64_encode_alloc(png_data, png_size);
    }

    if (settings.report_template_pskmap[0]) {
        configured_success_template = settings.report_template_pskmap;
    }
    if (settings.report_template_pskmap_failed[0]) {
        configured_failed_template = settings.report_template_pskmap_failed;
    }

    if (base64 && *base64) {
        size_t image_cq_cap = strlen(base64) + 32;
        image_cq = (char *)calloc(1, image_cq_cap);
        if (image_cq) {
            snprintf(image_cq, image_cq_cap, "[CQ:image,file=base64://%s]", base64);
        }
        if (!configured_success_template[0] ||
            (strcmp(normalized_band, "2m") == 0 &&
             strstr(configured_success_template, "6m") &&
             !strstr(configured_success_template, "{{pskmap_band}}"))) {
            configured_success_template = success_template;
        }
        message = render_pskmap_message_alloc(&settings, &snapshot, normalized_band, spot_count,
            configured_success_template,
            "", image_cq ? image_cq : "");
    } else {
        if (!detail[0]) {
            copy_string(detail, sizeof(detail), "图片编码失败");
        }
        if (!configured_failed_template[0] ||
            (strcmp(normalized_band, "2m") == 0 &&
             strstr(configured_failed_template, "6m") &&
             !strstr(configured_failed_template, "{{pskmap_band}}"))) {
            configured_failed_template = failed_template;
        }
        message = render_pskmap_message_alloc(&settings, &snapshot, normalized_band, spot_count,
            configured_failed_template,
            detail, "");
    }

    rc = onebot_send_message(app, type, target_id, message && message[0] ? message : "PSK 快照生成失败");
    app_log(app, rc == 0 ? "INFO" : "ERROR",
        "PSKReporter %s 图%s: target=%s spots=%d detail=%s",
        label_for_band(normalized_band),
        rc == 0 ? "发送成功" : "发送失败",
        target_id,
        spot_count,
        detail[0] ? detail : "-");

    free(message);
    free(image_cq);
    free(base64);
    free(png_data);
    return rc;
}
