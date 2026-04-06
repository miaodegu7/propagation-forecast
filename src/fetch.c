#include "app.h"

static int extract_tag_text(const char *xml, const char *tag, char *out, size_t out_len) {
    char open_tag[64];
    char close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *start = strstr(xml, open_tag);
    const char *end = NULL;
    if (!start) {
        return -1;
    }
    start += strlen(open_tag);
    end = strstr(start, close_tag);
    if (!end) {
        return -1;
    }
    size_t len = (size_t)(end - start);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    trim_whitespace(out);
    return 0;
}

static int extract_attr(const char *tag_start, const char *attr_name, char *out, size_t out_len) {
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=\"", attr_name);
    const char *p = strstr(tag_start, needle);
    if (!p) {
        return -1;
    }
    p += strlen(needle);
    const char *end = strchr(p, '"');
    if (!end) {
        return -1;
    }
    size_t len = (size_t)(end - p);
    if (len >= out_len) {
        len = out_len - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int parse_conditions(const char *xml, hamqsl_data_t *out) {
    const char *p = xml;
    out->band_count = 0;
    out->vhf_count = 0;

    while ((p = strstr(p, "<band ")) != NULL && out->band_count < MAX_BANDS) {
        const char *tag_end = strchr(p, '>');
        const char *close = strstr(p, "</band>");
        if (!tag_end || !close || close < tag_end) {
            break;
        }
        band_condition_t *band = &out->bands[out->band_count++];
        memset(band, 0, sizeof(*band));
        extract_attr(p, "name", band->name, sizeof(band->name));
        extract_attr(p, "time", band->time_slot, sizeof(band->time_slot));
        size_t len = (size_t)(close - (tag_end + 1));
        if (len >= sizeof(band->status)) {
            len = sizeof(band->status) - 1;
        }
        memcpy(band->status, tag_end + 1, len);
        band->status[len] = '\0';
        trim_whitespace(band->status);
        p = close + 7;
    }

    p = xml;
    while ((p = strstr(p, "<phenomenon ")) != NULL && out->vhf_count < MAX_VHF) {
        const char *tag_end = strchr(p, '>');
        const char *close = strstr(p, "</phenomenon>");
        if (!tag_end || !close || close < tag_end) {
            break;
        }
        vhf_condition_t *vhf = &out->vhf[out->vhf_count++];
        memset(vhf, 0, sizeof(*vhf));
        extract_attr(p, "name", vhf->name, sizeof(vhf->name));
        extract_attr(p, "location", vhf->location, sizeof(vhf->location));
        size_t len = (size_t)(close - (tag_end + 1));
        if (len >= sizeof(vhf->status)) {
            len = sizeof(vhf->status) - 1;
        }
        memcpy(vhf->status, tag_end + 1, len);
        vhf->status[len] = '\0';
        trim_whitespace(vhf->status);
        p = close + 13;
    }
    return 0;
}

int fetch_hamqsl_data(hamqsl_data_t *out) {
    memset(out, 0, sizeof(*out));
    long status = 0;
    char *xml = http_get_text("https://www.hamqsl.com/solarxml.php", NULL, &status);
    if (!xml || status < 200 || status >= 300) {
        free(xml);
        return -1;
    }

    char temp[128] = {0};
    if (extract_tag_text(xml, "updated", out->updated, sizeof(out->updated)) != 0) {
        free(xml);
        return -1;
    }
    temp[0] = '\0';
    extract_tag_text(xml, "solarflux", temp, sizeof(temp)); out->solarflux = atoi(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "aindex", temp, sizeof(temp)); out->aindex = atoi(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "kindex", temp, sizeof(temp)); out->kindex = atoi(temp);
    extract_tag_text(xml, "kindexnt", out->kindex_text, sizeof(out->kindex_text));
    extract_tag_text(xml, "xray", out->xray, sizeof(out->xray));
    temp[0] = '\0';
    extract_tag_text(xml, "sunspots", temp, sizeof(temp)); out->sunspots = atoi(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "heliumline", temp, sizeof(temp)); out->heliumline = atof(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "protonflux", temp, sizeof(temp)); out->protonflux = atoi(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "electonflux", temp, sizeof(temp)); out->electronflux = atoi(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "aurora", temp, sizeof(temp)); out->aurora = atoi(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "normalization", temp, sizeof(temp)); out->normalization = atof(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "latdegree", temp, sizeof(temp)); out->latdegree = atof(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "solarwind", temp, sizeof(temp)); out->solarwind = atof(temp);
    temp[0] = '\0';
    extract_tag_text(xml, "magneticfield", temp, sizeof(temp)); out->magneticfield = atof(temp);
    extract_tag_text(xml, "geomagfield", out->geomagfield, sizeof(out->geomagfield));
    extract_tag_text(xml, "signalnoise", out->signalnoise, sizeof(out->signalnoise));
    extract_tag_text(xml, "muf", out->muf, sizeof(out->muf));

    const char *source = strstr(xml, "<source");
    if (source) {
        extract_attr(source, "url", out->source_url, sizeof(out->source_url));
    }
    parse_conditions(xml, out);
    out->valid = 1;
    free(xml);
    return 0;
}

static const char *find_json_key(const char *json, const char *key) {
    static char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static int extract_json_object(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = find_json_key(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p, ':');
    if (!p) {
        return -1;
    }
    while (*p && *p != '{') {
        p++;
    }
    if (*p != '{') {
        return -1;
    }
    const char *start = p;
    int depth = 0;
    while (*p) {
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                size_t len = (size_t)(p - start + 1);
                if (len >= out_len) {
                    len = out_len - 1;
                }
                memcpy(out, start, len);
                out[len] = '\0';
                return 0;
            }
        }
        p++;
    }
    return -1;
}

static int extract_json_string(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = find_json_key(json, key);
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

static int extract_json_number(const char *json, const char *key, double *out) {
    const char *p = find_json_key(json, key);
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
    if (!(*p == '-' || *p == '+' || isdigit((unsigned char)*p))) {
        return -1;
    }
    char *end = NULL;
    *out = strtod(p, &end);
    return (end != p) ? 0 : -1;
}

static int extract_json_first_array_string(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = find_json_key(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p, '[');
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

static int extract_json_first_array_number(const char *json, const char *key, double *out) {
    const char *p = find_json_key(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p, '[');
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

int fetch_weather_data(const settings_t *settings, weather_data_t *out) {
    memset(out, 0, sizeof(*out));
    double latitude = settings->latitude;
    double longitude = settings->longitude;
    if ((latitude == 0.0 && longitude == 0.0) && settings->station_grid[0]) {
        grid_to_latlon(settings->station_grid, &latitude, &longitude);
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
    char *encoded_tz = curl_easy_escape(curl, settings->timezone[0] ? settings->timezone : "auto", 0);
    char url[1024];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?"
        "latitude=%.6f&longitude=%.6f&"
        "current=temperature_2m,relative_humidity_2m,dew_point_2m,pressure_msl,cloud_cover,visibility,wind_speed_10m,cape,lifted_index,precipitation_probability,is_day&"
        "daily=sunrise,sunset,daylight_duration,temperature_2m_max,temperature_2m_min,precipitation_probability_max&"
        "timezone=%s&forecast_days=1",
        latitude,
        longitude,
        encoded_tz ? encoded_tz : "auto");
    curl_free(encoded_tz);
    curl_easy_cleanup(curl);

    long status = 0;
    char *json = http_get_text(url, NULL, &status);
    if (!json || status < 200 || status >= 300) {
        free(json);
        return -1;
    }

    char current[4096];
    char daily[4096];
    if (extract_json_object(json, "current", current, sizeof(current)) != 0 ||
        extract_json_object(json, "daily", daily, sizeof(daily)) != 0) {
        free(json);
        return -1;
    }

    double number = 0.0;
    extract_json_string(current, "time", out->current_time, sizeof(out->current_time));
    if (extract_json_number(current, "is_day", &number) == 0) out->is_day = (int)number;
    if (extract_json_number(current, "temperature_2m", &number) == 0) out->temperature_c = number;
    if (extract_json_number(current, "relative_humidity_2m", &number) == 0) out->humidity = (int)number;
    if (extract_json_number(current, "dew_point_2m", &number) == 0) out->dewpoint_c = number;
    if (extract_json_number(current, "pressure_msl", &number) == 0) out->pressure_hpa = number;
    if (extract_json_number(current, "cloud_cover", &number) == 0) out->cloud_cover = (int)number;
    if (extract_json_number(current, "visibility", &number) == 0) out->visibility_m = number;
    if (extract_json_number(current, "wind_speed_10m", &number) == 0) out->wind_kmh = number;
    if (extract_json_number(current, "cape", &number) == 0) out->cape = number;
    if (extract_json_number(current, "lifted_index", &number) == 0) out->lifted_index = number;
    if (extract_json_number(current, "precipitation_probability", &number) == 0) out->precipitation_probability = (int)number;
    if (extract_json_first_array_string(daily, "sunrise", out->sunrise, sizeof(out->sunrise)) != 0) out->sunrise[0] = '\0';
    if (extract_json_first_array_string(daily, "sunset", out->sunset, sizeof(out->sunset)) != 0) out->sunset[0] = '\0';
    if (extract_json_first_array_number(daily, "daylight_duration", &number) == 0) out->daylight_hours = number / 3600.0;
    if (extract_json_first_array_number(daily, "temperature_2m_max", &number) == 0) out->tmax_c = number;
    if (extract_json_first_array_number(daily, "temperature_2m_min", &number) == 0) out->tmin_c = number;
    if (extract_json_first_array_number(daily, "precipitation_probability_max", &number) == 0) out->daily_precip_probability = (int)number;
    out->valid = 1;

    free(json);
    return 0;
}

static void json_escape_to_sb(sb_t *sb, const char *text) {
    const char *p = text ? text : "";
    while (*p) {
        switch (*p) {
            case '\\': sb_append(sb, "\\\\"); break;
            case '"': sb_append(sb, "\\\""); break;
            case '\n': sb_append(sb, "\\n"); break;
            case '\r': sb_append(sb, "\\r"); break;
            case '\t': sb_append(sb, "\\t"); break;
            default: {
                char tmp[2] = {*p, '\0'};
                sb_append(sb, tmp);
                break;
            }
        }
        p++;
    }
}

static int looks_numeric(const char *text) {
    if (!text || !*text) {
        return 0;
    }
    for (const char *p = text; *p; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return 0;
        }
    }
    return 1;
}

static void append_band_summary(sb_t *sb, const hamqsl_data_t *ham, const char *time_slot, const char *label) {
    sb_appendf(sb, "%s: ", label);
    int first = 1;
    for (int i = 0; i < ham->band_count; ++i) {
        if (strcasecmp(ham->bands[i].time_slot, time_slot) != 0) {
            continue;
        }
        if (!first) {
            sb_append(sb, " | ");
        }
        sb_appendf(sb, "%s %s", ham->bands[i].name, ham->bands[i].status);
        first = 0;
    }
    if (first) {
        sb_append(sb, "无数据");
    }
    sb_append(sb, "\n");
}

static void compose_sun_summary(const snapshot_t *snapshot, char *out, size_t out_len) {
    const hamqsl_data_t *ham = &snapshot->hamqsl;
    const weather_data_t *weather = &snapshot->weather;
    const char *phase = weather->is_day ? "白天" : "夜晚";
    snprintf(out, out_len,
        "太阳与空间天气：SFI %d，A %d，K %d，X-Ray %s，黑子 %d，太阳风 %.1f km/s，Bz %.1f nT；本地当前为%s，日出 %s，日落 %s。",
        ham->solarflux,
        ham->aindex,
        ham->kindex,
        ham->xray,
        ham->sunspots,
        ham->solarwind,
        ham->magneticfield,
        phase,
        weather->sunrise,
        weather->sunset);
}

static void compose_analysis(const snapshot_t *snapshot, char *out, size_t out_len) {
    const hamqsl_data_t *ham = &snapshot->hamqsl;
    const weather_data_t *weather = &snapshot->weather;
    const psk_summary_t *psk = &snapshot->psk;

    char line1[256];
    char line2[256];
    char line3[256];

    snprintf(line1, sizeof(line1),
        "HF 侧整体受 SFI=%d 与 K=%d 影响，当前地磁为 %s，20m/30m 通常仍然值得关注，高频高段则要看白天电离层支撑。",
        ham->solarflux, ham->kindex, ham->geomagfield);
    snprintf(line2, sizeof(line2),
        "6m 实测优先：近 %d 分钟本地相关 PSKReporter 6m spot %d 条，判断为“%s”，置信度 %s。",
        60, psk->local_spots_60m, psk->assessment, psk->confidence);
    snprintf(line3, sizeof(line3),
        "气象仅作辅助：温度 %.1fC、云量 %d%%、CAPE %.0f、Lifted Index %.1f，若午后到傍晚对流增强，可提高守听价值，但不能替代实测 spot。",
        weather->temperature_c, weather->cloud_cover, weather->cape, weather->lifted_index);

    snprintf(out, out_len, "%s\n%s\n%s", line1, line2, line3);
}

void build_reports(app_t *app, snapshot_t *snapshot) {
    (void)app;
    sb_t main_report;
    sb_t report_6m;
    sb_t report_solar;
    sb_init(&main_report);
    sb_init(&report_6m);
    sb_init(&report_solar);

    char refreshed[64];
    format_time_local(snapshot->refreshed_at, refreshed, sizeof(refreshed));
    compose_sun_summary(snapshot, snapshot->sun_summary, sizeof(snapshot->sun_summary));
    compose_analysis(snapshot, snapshot->analysis_summary, sizeof(snapshot->analysis_summary));

    sb_appendf(&main_report, "%s 传播简报\n", APP_NAME);
    sb_appendf(&main_report, "刷新时间: %s\n", refreshed);
    if (snapshot->hamqsl.valid) {
        sb_appendf(&main_report, "HAMqsl更新时间: %s\n", snapshot->hamqsl.updated);
        sb_appendf(&main_report,
            "太阳数据: SFI %d | A %d | K %d | X-Ray %s | 黑子 %d | 地磁 %s | 噪声 %s\n",
            snapshot->hamqsl.solarflux,
            snapshot->hamqsl.aindex,
            snapshot->hamqsl.kindex,
            snapshot->hamqsl.xray,
            snapshot->hamqsl.sunspots,
            snapshot->hamqsl.geomagfield,
            snapshot->hamqsl.signalnoise);
        append_band_summary(&main_report, &snapshot->hamqsl, "day", "HF日间");
        append_band_summary(&main_report, &snapshot->hamqsl, "night", "HF夜间");
    } else {
        sb_append(&main_report, "HAMqsl 数据暂不可用\n");
    }

    if (snapshot->weather.valid) {
        sb_appendf(&main_report,
            "本地太阳/天气: %s | 日出 %s | 日落 %s | 日照 %.1fh | %.1f/%.1fC | 湿度 %d%% | 气压 %.1fhPa\n",
            snapshot->weather.is_day ? "白天" : "夜晚",
            snapshot->weather.sunrise,
            snapshot->weather.sunset,
            snapshot->weather.daylight_hours,
            snapshot->weather.tmin_c,
            snapshot->weather.tmax_c,
            snapshot->weather.humidity,
            snapshot->weather.pressure_hpa);
    } else {
        sb_append(&main_report, "天气数据暂不可用\n");
    }

    sb_appendf(&main_report,
        "6m实测: 15分钟本地 %d 条 / 60分钟本地 %d 条 / 60分钟全局 %d 条 | 判定: %s | 置信度: %s | 分数: %d/100\n",
        snapshot->psk.local_spots_15m,
        snapshot->psk.local_spots_60m,
        snapshot->psk.global_spots_60m,
        snapshot->psk.assessment,
        snapshot->psk.confidence,
        snapshot->psk.score);
    if (snapshot->psk.latest_pair[0]) {
        sb_appendf(&main_report, "最新本地相关6m spot: %s @ %s\n", snapshot->psk.latest_pair, snapshot->psk.latest_local_time);
    }
    if (snapshot->psk.farthest_peer[0]) {
        sb_appendf(&main_report, "最远相关路径: %s %s，约 %d km\n",
            snapshot->psk.farthest_peer,
            snapshot->psk.farthest_grid,
            snapshot->psk.longest_path_km);
    }
    sb_append(&main_report, "分析:\n");
    sb_append(&main_report, snapshot->analysis_summary);
    sb_append(&main_report, "\n");

    sb_appendf(&report_6m, "6m 简报\n刷新时间: %s\n", refreshed);
    sb_appendf(&report_6m,
        "本地相关 spot: 15分钟 %d 条，60分钟 %d 条；全局 60分钟 %d 条。\n",
        snapshot->psk.local_spots_15m,
        snapshot->psk.local_spots_60m,
        snapshot->psk.global_spots_60m);
    sb_appendf(&report_6m, "判定: %s，置信度: %s，分数: %d/100。\n",
        snapshot->psk.assessment, snapshot->psk.confidence, snapshot->psk.score);
    if (snapshot->psk.latest_pair[0]) {
        sb_appendf(&report_6m, "最近相关 spot: %s @ %s\n", snapshot->psk.latest_pair, snapshot->psk.latest_local_time);
    }
    if (snapshot->weather.valid) {
        sb_appendf(&report_6m,
            "辅助天气: CAPE %.0f，LI %.1f，云量 %d%%，风速 %.1f km/h。气象仅作辅助参考。\n",
            snapshot->weather.cape,
            snapshot->weather.lifted_index,
            snapshot->weather.cloud_cover,
            snapshot->weather.wind_kmh);
    }

    sb_appendf(&report_solar, "太阳状态简报\n刷新时间: %s\n", refreshed);
    sb_appendf(&report_solar, "%s\n", snapshot->sun_summary);
    if (snapshot->hamqsl.valid) {
        sb_appendf(&report_solar,
            "更多: HeLine %.1f | Proton %d | Electron %d | Aurora %d | Noise %s | MUF %s\n",
            snapshot->hamqsl.heliumline,
            snapshot->hamqsl.protonflux,
            snapshot->hamqsl.electronflux,
            snapshot->hamqsl.aurora,
            snapshot->hamqsl.signalnoise,
            snapshot->hamqsl.muf[0] ? snapshot->hamqsl.muf : "NoRpt");
    }

    copy_string(snapshot->report_text, sizeof(snapshot->report_text), main_report.data ? main_report.data : "");
    copy_string(snapshot->report_6m, sizeof(snapshot->report_6m), report_6m.data ? report_6m.data : "");
    copy_string(snapshot->report_solar, sizeof(snapshot->report_solar), report_solar.data ? report_solar.data : "");

    sb_free(&main_report);
    sb_free(&report_6m);
    sb_free(&report_solar);
}

int refresh_snapshot(app_t *app, int force) {
    pthread_mutex_lock(&app->refresh_mutex);

    settings_t settings;
    storage_load_settings(app, &settings);
    apply_timezone(settings.timezone);

    snapshot_t next_snapshot;
    memset(&next_snapshot, 0, sizeof(next_snapshot));
    pthread_mutex_lock(&app->cache_mutex);
    next_snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    time_t now = time(NULL);
    if (!force && next_snapshot.refreshed_at > 0 &&
        difftime(now, next_snapshot.refreshed_at) < settings.refresh_interval_minutes * 60) {
        pthread_mutex_lock(&app->cache_mutex);
        app->settings = settings;
        pthread_mutex_unlock(&app->cache_mutex);
        pthread_mutex_unlock(&app->refresh_mutex);
        return 0;
    }

    hamqsl_data_t ham;
    weather_data_t weather;
    int ham_rc = fetch_hamqsl_data(&ham);
    int weather_rc = fetch_weather_data(&settings, &weather);
    if (ham_rc == 0) {
        next_snapshot.hamqsl = ham;
    }
    if (weather_rc == 0) {
        next_snapshot.weather = weather;
    }
    psk_compute_summary(app, &settings, &next_snapshot.psk);
    next_snapshot.refreshed_at = now;
    build_reports(app, &next_snapshot);

    pthread_mutex_lock(&app->cache_mutex);
    app->settings = settings;
    app->snapshot = next_snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    if (ham_rc == 0 || weather_rc == 0) {
        app_log(app, "INFO", "刷新传播数据完成: HAMqsl=%s weather=%s local6m=%d",
            ham_rc == 0 ? "ok" : "fail",
            weather_rc == 0 ? "ok" : "fail",
            next_snapshot.psk.local_spots_60m);
    } else {
        app_log(app, "WARN", "刷新传播数据失败，保留旧缓存");
    }

    pthread_mutex_unlock(&app->refresh_mutex);
    return (ham_rc == 0 || weather_rc == 0) ? 0 : -1;
}

int onebot_send_message(app_t *app, target_type_t type, const char *target_id, const char *message) {
    settings_t settings;
    storage_load_settings(app, &settings);
    if (!settings.onebot_api_base[0] || !target_id || !message) {
        return -1;
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/%s",
        settings.onebot_api_base,
        type == TARGET_PRIVATE ? "send_private_msg" : "send_group_msg");

    sb_t json;
    sb_init(&json);
    sb_append(&json, "{");
    if (type == TARGET_PRIVATE) {
        if (looks_numeric(target_id)) {
            sb_appendf(&json, "\"user_id\":%s,", target_id);
        } else {
            sb_append(&json, "\"user_id\":\"");
            json_escape_to_sb(&json, target_id);
            sb_append(&json, "\",");
        }
    } else {
        if (looks_numeric(target_id)) {
            sb_appendf(&json, "\"group_id\":%s,", target_id);
        } else {
            sb_append(&json, "\"group_id\":\"");
            json_escape_to_sb(&json, target_id);
            sb_append(&json, "\",");
        }
    }
    sb_append(&json, "\"message\":\"");
    json_escape_to_sb(&json, message);
    sb_append(&json, "\",\"auto_escape\":true}");

    long status = 0;
    char *response = http_post_json(url, settings.onebot_access_token, json.data ? json.data : "{}", &status);
    int ok = (response != NULL && status >= 200 && status < 300);
    if (!ok) {
        app_log(app, "ERROR", "OneBot 发送失败: target=%s status=%ld", target_id, status);
    }
    free(response);
    sb_free(&json);
    return ok ? 0 : -1;
}

int send_report_to_all_targets(app_t *app, const char *message) {
    target_t targets[MAX_TARGETS];
    int count = 0;
    storage_load_targets(app, targets, MAX_TARGETS, &count);
    int sent = 0;
    int failed = 0;
    for (int i = 0; i < count; ++i) {
        if (!targets[i].enabled) {
            continue;
        }
        if (onebot_send_message(app, targets[i].type, targets[i].target_id, message) == 0) {
            sent++;
        } else {
            failed++;
        }
    }
    app_log(app, failed ? "WARN" : "INFO", "群发完成: success=%d failed=%d", sent, failed);
    return sent > 0 ? sent : -1;
}
