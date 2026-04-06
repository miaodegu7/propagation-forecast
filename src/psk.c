#include "app.h"

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
        app->mqtt_connected = 1;
        mosquitto_subscribe(mosq, NULL, "pskr/filter/v2/6m/#", 0);
        app_log(app, "INFO", "已连接 PSKReporter MQTT");
    } else {
        app->mqtt_connected = 0;
        app_log(app, "WARN", "PSKReporter MQTT 连接失败 rc=%d", rc);
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

    psk_spot_t spot;
    memset(&spot, 0, sizeof(spot));
    copy_string(spot.band, sizeof(spot.band), "6m");

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
    if (!spot.timestamp) {
        spot.timestamp = time(NULL);
    }
    psk_add_spot(app, &spot);
}

int psk_start(app_t *app) {
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
                             char *matched_prefix, size_t matched_prefix_len) {
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
    if ((direct_match || sender_near) && receiver_ok) {
        *counterpart_distance = (int)round(haversine_km(station_lat, station_lon, receiver_lat, receiver_lon));
        copy_string(peer_call, peer_call_len, spot->receiver_call);
        copy_string(peer_grid, peer_grid_len, spot->receiver_grid);
    } else if ((direct_match || receiver_near) && sender_ok) {
        *counterpart_distance = (int)round(haversine_km(station_lat, station_lon, sender_lat, sender_lon));
        copy_string(peer_call, peer_call_len, spot->sender_call);
        copy_string(peer_grid, peer_grid_len, spot->sender_grid);
    }
    return 1;
}

void psk_compute_summary(app_t *app, const settings_t *settings, psk_summary_t *out) {
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
                matched_prefix, sizeof(matched_prefix));
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

    if (out->local_spots_15m >= 3 || out->local_spots_60m >= 6) {
        copy_string(out->assessment, sizeof(out->assessment), "明确开口");
        copy_string(out->confidence, sizeof(out->confidence), "高");
    } else if (out->local_spots_60m >= 2) {
        copy_string(out->assessment, sizeof(out->assessment), "有开口迹象");
        copy_string(out->confidence, sizeof(out->confidence), "中");
    } else if (out->global_spots_60m >= 20) {
        copy_string(out->assessment, sizeof(out->assessment), "全球活跃，本地待观察");
        copy_string(out->confidence, sizeof(out->confidence), "中低");
    } else if (out->mqtt_connected) {
        copy_string(out->assessment, sizeof(out->assessment), "暂未见本地开口");
        copy_string(out->confidence, sizeof(out->confidence), "低");
    } else {
        copy_string(out->assessment, sizeof(out->assessment), "实时数据未连接");
        copy_string(out->confidence, sizeof(out->confidence), "未知");
    }
}

void psk_append_recent_rows(app_t *app, sb_t *rows, const settings_t *settings, int max_rows) {
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
        int peer_distance = 0;
        char peer_call[MAX_TEXT] = {0};
        char peer_grid[MAX_TEXT] = {0};
        char matched_prefix[MAX_TEXT] = {0};
        if (!is_local_relevant(settings, spot, station_lat, station_lon,
                &peer_distance, peer_call, sizeof(peer_call), peer_grid, sizeof(peer_grid),
                matched_prefix, sizeof(matched_prefix))) {
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
        sb_append(rows, "<tr><td colspan=\"7\">最近窗口内还没有本地相关 6m spot</td></tr>");
    }
}
