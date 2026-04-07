#include "app.h"

static void json_escape_to_sb(sb_t *sb, const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    while (*p) {
        switch (*p) {
            case '\\': sb_append(sb, "\\\\"); break;
            case '"': sb_append(sb, "\\\""); break;
            case '\n': sb_append(sb, "\\n"); break;
            case '\r': sb_append(sb, "\\r"); break;
            case '\t': sb_append(sb, "\\t"); break;
            default: {
                char temp[2] = {(char)*p, '\0'};
                sb_append(sb, temp);
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
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (!isdigit(*p)) {
            return 0;
        }
    }
    return 1;
}

static int geomag_g_from_k_runtime(int kindex) {
    if (kindex >= 9) return 5;
    if (kindex >= 8) return 4;
    if (kindex >= 7) return 3;
    if (kindex >= 6) return 2;
    if (kindex >= 5) return 1;
    return 0;
}

static int sixm_alert_level(const snapshot_t *snapshot, const settings_t *settings) {
    int psk_hot = snapshot->psk.local_spots_15m >= settings->sixm_psk_trigger_spots ||
        snapshot->psk.local_spots_60m >= settings->sixm_psk_trigger_spots;
    int psk_some = snapshot->psk.local_spots_60m > 0;
    int tropo_hot = snapshot->tropo.valid && snapshot->tropo.score >= 78;
    int tropo_some = snapshot->tropo.valid && snapshot->tropo.score >= 55;
    int weather_hot = snapshot->weather.valid && snapshot->weather.sixm_weather_score >= 70;
    int weather_some = snapshot->weather.valid && snapshot->weather.sixm_weather_score >= 50;

    if (psk_hot && (tropo_some || weather_some)) return 3;
    if (psk_hot || (psk_some && (tropo_some || weather_some)) || (tropo_hot && weather_hot)) return 2;
    if (psk_some || tropo_some || weather_some) return 1;
    return 0;
}

static const char *meteor_fetch_status(const settings_t *settings, const meteor_data_t *meteor, int rc) {
    if (rc != 0) {
        return "fail";
    }
    if (!settings->meteor_enabled) {
        return "skip";
    }
    if (meteor->shower_name[0]) {
        return "ok";
    }
    return "empty";
}

static const char *satellite_fetch_status(const settings_t *settings, const satellite_summary_t *satellite, int rc) {
    if (rc != 0) {
        return "fail";
    }
    if (!settings->satellite_enabled || !settings->satellite_api_base[0] || !settings->satellite_api_key[0]) {
        return "skip";
    }
    if (satellite->pass_count > 0) {
        return "ok";
    }
    return "empty";
}

static void ensure_poll_state(poll_state_t *state, int interval_seconds, time_t now) {
    interval_seconds = clamp_int(interval_seconds, 1, 365 * 24 * 3600);
    if (state->interval_seconds > 0 && state->interval_seconds != interval_seconds && state->next_due > now) {
        time_t remaining = state->next_due - now;
        if (remaining > interval_seconds) {
            state->next_due = now + interval_seconds;
        }
    }
    state->interval_seconds = interval_seconds;
}

static int poll_due(poll_state_t *state, int interval_seconds, time_t now) {
    ensure_poll_state(state, interval_seconds, now);
    return state->next_due == 0 || now >= state->next_due;
}

static void mark_poll_done(poll_state_t *state, time_t now) {
    int interval = state->interval_seconds > 0 ? state->interval_seconds : 60;
    state->next_due = now + interval;
}

const char *app_get_report_by_kind(const snapshot_t *snapshot, const char *kind) {
    if (!kind || !*kind || strcmp(kind, "full") == 0) return snapshot->report_text;
    if (strcmp(kind, "6m") == 0) return snapshot->report_6m;
    if (strcmp(kind, "solar") == 0) return snapshot->report_solar;
    if (strcmp(kind, "geomag") == 0) return snapshot->report_geomag;
    if (strcmp(kind, "open6m") == 0) return snapshot->report_open6m;
    if (strcmp(kind, "help") == 0) return snapshot->report_help;
    return snapshot->report_text;
}

int onebot_send_message(app_t *app, target_type_t type, const char *target_id, const char *message) {
    settings_t settings;
    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    pthread_mutex_unlock(&app->cache_mutex);
    if (!settings.onebot_api_base[0] || !target_id || !message || !*message) {
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
    sb_append(&json, "\",\"auto_escape\":false}");

    int attempts = settings.onebot_retry_count + 1;
    int ok = 0;
    long last_status = 0;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        long status = 0;
        char *response = http_post_json(url, settings.onebot_access_token, json.data ? json.data : "{}", &status);
        ok = response != NULL && status >= 200 && status < 300;
        last_status = status;
        free(response);
        if (ok) {
            break;
        }
        if (attempt < attempts && settings.onebot_retry_delay_ms > 0) {
            app_sleep_ms(settings.onebot_retry_delay_ms);
        }
    }
    if (!ok) {
        app_log(app, "ERROR", "OneBot 发送失败: target=%s status=%ld retries=%d",
            target_id, last_status, settings.onebot_retry_count);
    }
    sb_free(&json);
    return ok ? 0 : -1;
}

int send_report_to_all_targets(app_t *app, const char *message) {
    target_t targets[MAX_TARGETS];
    int count = 0;
    settings_t settings;
    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    pthread_mutex_unlock(&app->cache_mutex);
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
        if (i + 1 < count && settings.onebot_send_delay_ms > 0) {
            app_sleep_ms(settings.onebot_send_delay_ms);
        }
    }
    app_log(app, failed ? "WARN" : "INFO", "群发完成: success=%d failed=%d", sent, failed);
    return sent > 0 ? sent : -1;
}

int send_report_kind_to_all_targets(app_t *app, const char *report_kind) {
    refresh_snapshot(app, 0);
    pthread_mutex_lock(&app->cache_mutex);
    snapshot_t snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);
    return send_report_to_all_targets(app, app_get_report_by_kind(&snapshot, report_kind));
}

void app_rebuild_snapshot(app_t *app) {
    pthread_mutex_lock(&app->refresh_mutex);
    settings_t settings;
    storage_load_settings(app, &settings);
    apply_timezone(settings.timezone);

    snapshot_t next_snapshot;
    pthread_mutex_lock(&app->cache_mutex);
    next_snapshot = app->snapshot;
    app->settings = settings;
    pthread_mutex_unlock(&app->cache_mutex);

    psk_compute_summary(app, &settings, &next_snapshot.psk);
    next_snapshot.refreshed_at = time(NULL);
    build_reports(app, &next_snapshot);

    pthread_mutex_lock(&app->cache_mutex);
    app->snapshot = next_snapshot;
    pthread_mutex_unlock(&app->cache_mutex);
    pthread_mutex_unlock(&app->refresh_mutex);
}

int app_force_refresh(app_t *app) {
    pthread_mutex_lock(&app->refresh_mutex);
    settings_t settings;
    storage_load_settings(app, &settings);
    apply_timezone(settings.timezone);

    snapshot_t next_snapshot;
    pthread_mutex_lock(&app->cache_mutex);
    next_snapshot = app->snapshot;
    app->settings = settings;
    pthread_mutex_unlock(&app->cache_mutex);

    hamqsl_data_t ham;
    weather_data_t weather;
    tropo_data_t tropo;
    meteor_data_t meteor;
    satellite_summary_t satellite;

    int ham_rc = fetch_hamqsl_data(&ham);
    int weather_rc = fetch_weather_data(&settings, &weather);
    int tropo_rc = fetch_tropo_data(&settings, &tropo);
    int meteor_rc = fetch_meteor_data(&settings, &meteor);
    int satellite_rc = fetch_satellite_data(&settings, &satellite, app);

    if (ham_rc == 0) next_snapshot.hamqsl = ham;
    if (weather_rc == 0) next_snapshot.weather = weather;
    if (tropo_rc == 0) next_snapshot.tropo = tropo;
    if (meteor_rc == 0) next_snapshot.meteor = meteor;
    if (satellite_rc == 0) next_snapshot.satellite = satellite;

    psk_compute_summary(app, &settings, &next_snapshot.psk);
    next_snapshot.refreshed_at = time(NULL);
    build_reports(app, &next_snapshot);

    pthread_mutex_lock(&app->cache_mutex);
    app->snapshot = next_snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    app_log(app, "INFO",
        "强制刷新完成: ham=%s weather=%s tropo=%s meteor=%s sat=%s local6m=%d",
        ham_rc == 0 ? "ok" : "fail",
        weather_rc == 0 ? "ok" : "fail",
        tropo_rc == 0 ? "ok" : "fail",
        meteor_fetch_status(&settings, &meteor, meteor_rc),
        satellite_fetch_status(&settings, &satellite, satellite_rc),
        next_snapshot.psk.local_spots_60m);

    pthread_mutex_unlock(&app->refresh_mutex);
    return 0;
}

void app_run_periodic_fetches(app_t *app) {
    pthread_mutex_lock(&app->refresh_mutex);
    settings_t settings;
    storage_load_settings(app, &settings);
    apply_timezone(settings.timezone);

    snapshot_t next_snapshot;
    pthread_mutex_lock(&app->cache_mutex);
    next_snapshot = app->snapshot;
    app->settings = settings;
    pthread_mutex_unlock(&app->cache_mutex);

    time_t now = time(NULL);
    int changed = 0;
    int psk_changed = 0;

    if (poll_due(&app->hamqsl_poll, settings.hamqsl_interval_minutes * 60, now)) {
        hamqsl_data_t ham;
        if (fetch_hamqsl_data(&ham) == 0) {
            next_snapshot.hamqsl = ham;
            changed = 1;
        }
        mark_poll_done(&app->hamqsl_poll, now);
    }
    if (poll_due(&app->weather_poll, settings.weather_interval_minutes * 60, now)) {
        weather_data_t weather;
        if (fetch_weather_data(&settings, &weather) == 0) {
            next_snapshot.weather = weather;
            changed = 1;
        }
        mark_poll_done(&app->weather_poll, now);
    }
    if (poll_due(&app->tropo_poll, settings.tropo_interval_minutes * 60, now)) {
        tropo_data_t tropo;
        if (fetch_tropo_data(&settings, &tropo) == 0) {
            next_snapshot.tropo = tropo;
            changed = 1;
        }
        mark_poll_done(&app->tropo_poll, now);
    }
    if (poll_due(&app->meteor_poll, settings.meteor_interval_hours * 3600, now)) {
        meteor_data_t meteor;
        if (fetch_meteor_data(&settings, &meteor) == 0) {
            next_snapshot.meteor = meteor;
            changed = 1;
        }
        mark_poll_done(&app->meteor_poll, now);
    }
    if (poll_due(&app->satellite_poll, settings.satellite_interval_hours * 3600, now)) {
        satellite_summary_t satellite;
        if (fetch_satellite_data(&settings, &satellite, app) == 0) {
            next_snapshot.satellite = satellite;
            changed = 1;
        }
        mark_poll_done(&app->satellite_poll, now);
    }
    if (poll_due(&app->psk_eval_poll, settings.psk_eval_interval_seconds, now)) {
        psk_compute_summary(app, &settings, &next_snapshot.psk);
        mark_poll_done(&app->psk_eval_poll, now);
        changed = 1;
        psk_changed = 1;
    }
    if (changed || poll_due(&app->snapshot_poll, settings.snapshot_rebuild_seconds, now)) {
        if (!psk_changed) {
            psk_compute_summary(app, &settings, &next_snapshot.psk);
        }
        next_snapshot.refreshed_at = now;
        build_reports(app, &next_snapshot);
        mark_poll_done(&app->snapshot_poll, now);
        pthread_mutex_lock(&app->cache_mutex);
        app->snapshot = next_snapshot;
        pthread_mutex_unlock(&app->cache_mutex);
    }

    pthread_mutex_unlock(&app->refresh_mutex);
}

void app_check_alerts(app_t *app) {
    settings_t settings;
    snapshot_t snapshot;
    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    if (app->last_geomag_alert_g == 0 && app->last_sixm_alert_level == 0 && app->last_sixm_alert_at == 0) {
        char temp[64];
        storage_get_state(app, "last_geomag_alert_g", temp, sizeof(temp));
        app->last_geomag_alert_g = atoi(temp);
        storage_get_state(app, "last_6m_alert_level", temp, sizeof(temp));
        app->last_sixm_alert_level = atoi(temp);
        storage_get_state(app, "last_6m_alert_at", temp, sizeof(temp));
        app->last_sixm_alert_at = (time_t)atoll(temp);
    }

    int current_g = geomag_g_from_k_runtime(snapshot.hamqsl.kindex);
    if (settings.geomag_alert_enabled && current_g >= settings.geomag_alert_threshold_g) {
        if (current_g > app->last_geomag_alert_g && snapshot.report_geomag[0]) {
            send_report_to_all_targets(app, snapshot.report_geomag);
            char temp[32];
            snprintf(temp, sizeof(temp), "%d", current_g);
            storage_set_state(app, "last_geomag_alert_g", temp);
            app->last_geomag_alert_g = current_g;
        }
    } else if (app->last_geomag_alert_g != 0) {
        storage_set_state(app, "last_geomag_alert_g", "0");
        app->last_geomag_alert_g = 0;
    }

    int level = sixm_alert_level(&snapshot, &settings);
    time_t now = time(NULL);
    if (settings.sixm_alert_enabled && level > 0) {
        if (level > app->last_sixm_alert_level ||
            difftime(now, app->last_sixm_alert_at) >= settings.sixm_alert_interval_minutes * 60) {
            if (snapshot.report_open6m[0]) {
                send_report_to_all_targets(app, snapshot.report_open6m);
                char temp[32];
                snprintf(temp, sizeof(temp), "%d", level);
                storage_set_state(app, "last_6m_alert_level", temp);
                snprintf(temp, sizeof(temp), "%lld", (long long)now);
                storage_set_state(app, "last_6m_alert_at", temp);
                app->last_sixm_alert_level = level;
                app->last_sixm_alert_at = now;
            }
        }
    } else if (app->last_sixm_alert_level != 0 || app->last_sixm_alert_at != 0) {
        storage_set_state(app, "last_6m_alert_level", "0");
        storage_set_state(app, "last_6m_alert_at", "0");
        app->last_sixm_alert_level = 0;
        app->last_sixm_alert_at = 0;
    }
}

int app_rate_limit_allow(app_t *app, const char *key) {
    settings_t settings;
    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    pthread_mutex_unlock(&app->cache_mutex);
    if (settings.rate_limit_per_minute <= 0) {
        return 1;
    }

    time_t minute_window = time(NULL) / 60;
    pthread_mutex_lock(&app->rate_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_RATE_LIMITS; ++i) {
        if (app->rate_limits[i].key[0] == '\0' && slot < 0) {
            slot = i;
        }
        if (strcmp(app->rate_limits[i].key, key) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    rate_limit_entry_t *entry = &app->rate_limits[slot];
    if (strcmp(entry->key, key) != 0) {
        copy_string(entry->key, sizeof(entry->key), key);
        entry->minute_window = minute_window;
        entry->count = 0;
    }
    if (entry->minute_window != minute_window) {
        entry->minute_window = minute_window;
        entry->count = 0;
    }
    int allowed = entry->count < settings.rate_limit_per_minute;
    if (allowed) {
        entry->count++;
    }
    pthread_mutex_unlock(&app->rate_mutex);
    return allowed;
}

int refresh_snapshot(app_t *app, int force) {
    if (force) {
        return app_force_refresh(app);
    }
    app_run_periodic_fetches(app);
    app_rebuild_snapshot(app);
    return 0;
}
