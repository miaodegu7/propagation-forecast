#include "app.h"

static int exec_sql(sqlite3 *db, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "sqlite error: %s\n", errmsg ? errmsg : "(unknown)");
        sqlite3_free(errmsg);
    }
    return rc;
}

static int upsert_default(sqlite3 *db, const char *key, const char *value) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO settings(key, value) VALUES(?, ?)";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? SQLITE_OK : rc;
}

static int load_setting_text(sqlite3 *db, const char *key, char *out, size_t out_len) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM settings WHERE key = ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        copy_string(out, out_len, (const char *)sqlite3_column_text(stmt, 0));
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_OK ? SQLITE_OK : SQLITE_NOTFOUND;
}

static void load_text_or_default(sqlite3 *db, const char *key, char *out, size_t out_len, const char *fallback) {
    if (load_setting_text(db, key, out, out_len) != SQLITE_OK) {
        copy_string(out, out_len, fallback);
    }
}

static int load_int_or_default(sqlite3 *db, const char *key, int fallback) {
    char temp[128];
    if (load_setting_text(db, key, temp, sizeof(temp)) == SQLITE_OK) {
        return atoi(temp);
    }
    return fallback;
}

static double load_double_or_default(sqlite3 *db, const char *key, double fallback) {
    char temp[128];
    if (load_setting_text(db, key, temp, sizeof(temp)) == SQLITE_OK) {
        return atof(temp);
    }
    return fallback;
}

static int seed_defaults(sqlite3 *db) {
    struct {
        const char *key;
        const char *value;
    } defaults[] = {
        {"bind_addr", "0.0.0.0"},
        {"http_port", "8080"},
        {"admin_user", ""},
        {"admin_password", ""},
        {"station_name", "BG0XXX"},
        {"station_grid", "PM01"},
        {"psk_grids", "PM01"},
        {"latitude", "31.2304"},
        {"longitude", "121.4737"},
        {"altitude_m", "4"},
        {"timezone", "Asia/Shanghai"},
        {"onebot_api_base", "http://127.0.0.1:3000"},
        {"onebot_access_token", ""},
        {"onebot_webhook_token", "change-me"},
        {"bot_name", "传播助手"},
        {"bot_qq", ""},
        {"bot_password", ""},
        {"onebot_send_delay_ms", "1200"},
        {"onebot_retry_count", "1"},
        {"onebot_retry_delay_ms", "2500"},
        {"hamqsl_interval_minutes", "30"},
        {"weather_interval_minutes", "30"},
        {"tropo_interval_minutes", "60"},
        {"meteor_interval_hours", "12"},
        {"satellite_interval_hours", "6"},
        {"psk_eval_interval_seconds", "60"},
        {"snapshot_rebuild_seconds", "60"},
        {"psk_radius_km", "900"},
        {"psk_window_minutes", "60"},
        {"rate_limit_per_minute", "6"},
        {"geomag_alert_enabled", "1"},
        {"geomag_alert_threshold_g", "2"},
        {"sixm_alert_enabled", "1"},
        {"sixm_alert_interval_minutes", "30"},
        {"sixm_psk_trigger_spots", "2"},
        {"tropo_source_url", "https://tropo.f5len.org/asia/"},
        {"tropo_forecast_hours", "24"},
        {"tropo_send_image", "1"},
        {"meteor_source_url", "https://www.imo.net/"},
        {"meteor_enabled", "1"},
        {"satellite_source_url", "https://api.n2yo.com/rest/v1/satellite/"},
        {"satellite_api_base", "https://api.n2yo.com/rest/v1/satellite/"},
        {"satellite_api_key", ""},
        {"satellite_enabled", "1"},
        {"satellite_days", "1"},
        {"satellite_min_elevation", "20"},
        {"satellite_window_start", "08:00"},
        {"satellite_window_end", "19:00"},
        {"satellite_mode_filter", "全部"},
        {"satellite_max_items", "8"},
        {"hamqsl_widget_url", "https://www.hamqsl.com/solar101sc.php"},
        {"hamqsl_selected_fields", "solarflux,aindex,kindex,xray,sunspots,solarwind,magneticfield,geomagfield,signalnoise,muf,fof2,muffactor,aurora"},
        {"include_source_urls", "1"},
        {"include_hamqsl_widget", "1"},
        {"report_template_full",
            "{{bot_name}} {{station_name}}({{station_grid}}) 每日传播简报\n"
            "{{section_hamqsl}}\n{{section_weather}}\n{{section_tropo}}\n{{section_6m}}\n{{section_meteor}}\n{{section_satellite}}\n{{section_sources}}"},
        {"report_template_6m",
            "{{bot_name}} 6米传播提醒\n{{section_tropo}}\n{{section_weather}}\n{{section_6m}}\n{{section_satellite}}\n{{section_sources}}"},
        {"report_template_solar",
            "{{bot_name}} 太阳与空间天气\n{{section_solar}}\n{{section_hamqsl}}\n{{section_sources}}"},
        {"report_template_geomag",
            "{{bot_name}} 地磁告警\n当前达到 G{{geomag_g}} 级，K={{ham_kindex}}，A={{ham_aindex}}，地磁状态：{{ham_geomagfield}}。请注意高频与极光相关传播变化。\n{{section_sources}}"},
        {"report_template_open6m",
            "{{bot_name}} 6米开口提醒\n级别：{{sixm_alert_level}}\n{{section_tropo}}\n{{section_weather}}\n{{section_6m}}\n{{section_sources}}"},
        {"help_template",
            "可用关键词：传播 / 6米 / 太阳 / 帮助\n"
            "当前机器人：{{bot_name}}\n"
            "台站：{{station_name}} {{station_grid}}\n"
            "PSK监控网格：{{psk_grids}}"},
        {"trigger_full", "传播,预报,简报"},
        {"trigger_6m", "6m,6米,六米"},
        {"trigger_solar", "太阳,磁暴,空间天气"},
        {"trigger_help", "帮助,help,菜单"}
    };

    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        int rc = upsert_default(db, defaults[i].key, defaults[i].value);
        if (rc != SQLITE_OK) {
            return rc;
        }
    }

    exec_sql(db,
        "INSERT OR IGNORE INTO schedule_rules(id, label, report_kind, hhmm, enabled, last_fire_date) "
        "VALUES(1, '早报', 'full', '08:30', 1, '');"
        "INSERT OR IGNORE INTO schedule_rules(id, label, report_kind, hhmm, enabled, last_fire_date) "
        "VALUES(2, '晚报', 'full', '20:30', 1, '');"
        "INSERT OR IGNORE INTO satellites(id, name, norad_id, mode_type, enabled, notes) "
        "VALUES(1, 'FO-29', 24278, '线性', 1, '经典线性转发器');"
        "INSERT OR IGNORE INTO satellites(id, name, norad_id, mode_type, enabled, notes) "
        "VALUES(2, 'SO-50', 27607, '非线性', 1, 'FM卫星');"
        "INSERT OR IGNORE INTO satellites(id, name, norad_id, mode_type, enabled, notes) "
        "VALUES(3, 'RS-44', 44909, '线性', 1, '高轨线性');"
        "INSERT OR IGNORE INTO satellites(id, name, norad_id, mode_type, enabled, notes) "
        "VALUES(4, 'CAS-4A', 42761, '线性', 0, '可按需启用');"
        "INSERT OR IGNORE INTO satellites(id, name, norad_id, mode_type, enabled, notes) "
        "VALUES(5, 'XW-2F', 40911, '线性', 0, '可按需启用');"
        "INSERT OR IGNORE INTO state(key, value) VALUES('last_geomag_alert_g', '0');"
        "INSERT OR IGNORE INTO state(key, value) VALUES('last_6m_alert_level', '0');"
        "INSERT OR IGNORE INTO state(key, value) VALUES('last_6m_alert_at', '0');");
    return SQLITE_OK;
}

int storage_init(app_t *app, const char *db_path) {
    int rc = sqlite3_open(db_path, &app->db);
    if (rc != SQLITE_OK) {
        return -1;
    }

    exec_sql(app->db,
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS settings ("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS targets ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " label TEXT NOT NULL,"
        " type TEXT NOT NULL,"
        " target_id TEXT NOT NULL,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " command_enabled INTEGER NOT NULL DEFAULT 1,"
        " notes TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS schedule_rules ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " label TEXT NOT NULL,"
        " report_kind TEXT NOT NULL,"
        " hhmm TEXT NOT NULL,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " last_fire_date TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS satellites ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL,"
        " norad_id INTEGER NOT NULL,"
        " mode_type TEXT NOT NULL DEFAULT '全部',"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " notes TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS state ("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS logs ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " created_at TEXT NOT NULL,"
        " level TEXT NOT NULL,"
        " message TEXT NOT NULL"
        ");");

    if (seed_defaults(app->db) != SQLITE_OK) {
        return -1;
    }
    return 0;
}

int storage_load_settings(app_t *app, settings_t *out) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&app->db_mutex);

    load_text_or_default(app->db, "bind_addr", out->bind_addr, sizeof(out->bind_addr), "0.0.0.0");
    out->http_port = load_int_or_default(app->db, "http_port", 8080);
    load_text_or_default(app->db, "admin_user", out->admin_user, sizeof(out->admin_user), "");
    load_text_or_default(app->db, "admin_password", out->admin_password, sizeof(out->admin_password), "");
    load_text_or_default(app->db, "station_name", out->station_name, sizeof(out->station_name), "BG0XXX");
    load_text_or_default(app->db, "station_grid", out->station_grid, sizeof(out->station_grid), "PM01");
    load_text_or_default(app->db, "psk_grids", out->psk_grids, sizeof(out->psk_grids), "PM01");
    out->latitude = load_double_or_default(app->db, "latitude", 31.2304);
    out->longitude = load_double_or_default(app->db, "longitude", 121.4737);
    out->altitude_m = load_double_or_default(app->db, "altitude_m", 4.0);
    load_text_or_default(app->db, "timezone", out->timezone, sizeof(out->timezone), "Asia/Shanghai");

    load_text_or_default(app->db, "onebot_api_base", out->onebot_api_base, sizeof(out->onebot_api_base), "http://127.0.0.1:3000");
    load_text_or_default(app->db, "onebot_access_token", out->onebot_access_token, sizeof(out->onebot_access_token), "");
    load_text_or_default(app->db, "onebot_webhook_token", out->onebot_webhook_token, sizeof(out->onebot_webhook_token), "change-me");
    load_text_or_default(app->db, "bot_name", out->bot_name, sizeof(out->bot_name), "传播助手");
    load_text_or_default(app->db, "bot_qq", out->bot_qq, sizeof(out->bot_qq), "");
    load_text_or_default(app->db, "bot_password", out->bot_password, sizeof(out->bot_password), "");
    out->onebot_send_delay_ms = load_int_or_default(app->db, "onebot_send_delay_ms", 1200);
    out->onebot_retry_count = load_int_or_default(app->db, "onebot_retry_count", 1);
    out->onebot_retry_delay_ms = load_int_or_default(app->db, "onebot_retry_delay_ms", 2500);
    load_text_or_default(app->db, "schedule_morning", out->schedule_morning, sizeof(out->schedule_morning), "08:30");
    load_text_or_default(app->db, "schedule_evening", out->schedule_evening, sizeof(out->schedule_evening), "20:30");
    out->morning_enabled = load_int_or_default(app->db, "morning_enabled", 1);
    out->evening_enabled = load_int_or_default(app->db, "evening_enabled", 1);
    out->refresh_interval_minutes = load_int_or_default(app->db, "refresh_interval_minutes", 15);

    out->hamqsl_interval_minutes = load_int_or_default(app->db, "hamqsl_interval_minutes", 30);
    out->weather_interval_minutes = load_int_or_default(app->db, "weather_interval_minutes", 30);
    out->tropo_interval_minutes = load_int_or_default(app->db, "tropo_interval_minutes", 60);
    out->meteor_interval_hours = load_int_or_default(app->db, "meteor_interval_hours", 12);
    out->satellite_interval_hours = load_int_or_default(app->db, "satellite_interval_hours", 6);
    out->psk_eval_interval_seconds = load_int_or_default(app->db, "psk_eval_interval_seconds", 60);
    out->snapshot_rebuild_seconds = load_int_or_default(app->db, "snapshot_rebuild_seconds", 60);

    out->psk_radius_km = load_int_or_default(app->db, "psk_radius_km", 900);
    out->psk_window_minutes = load_int_or_default(app->db, "psk_window_minutes", 60);
    out->rate_limit_per_minute = load_int_or_default(app->db, "rate_limit_per_minute", 6);

    out->geomag_alert_enabled = load_int_or_default(app->db, "geomag_alert_enabled", 1);
    out->geomag_alert_threshold_g = load_int_or_default(app->db, "geomag_alert_threshold_g", 2);
    out->sixm_alert_enabled = load_int_or_default(app->db, "sixm_alert_enabled", 1);
    out->sixm_alert_interval_minutes = load_int_or_default(app->db, "sixm_alert_interval_minutes", 30);
    out->sixm_psk_trigger_spots = load_int_or_default(app->db, "sixm_psk_trigger_spots", 2);

    load_text_or_default(app->db, "tropo_source_url", out->tropo_source_url, sizeof(out->tropo_source_url), "https://tropo.f5len.org/asia/");
    out->tropo_forecast_hours = load_int_or_default(app->db, "tropo_forecast_hours", 24);
    out->tropo_send_image = load_int_or_default(app->db, "tropo_send_image", 1);

    load_text_or_default(app->db, "meteor_source_url", out->meteor_source_url, sizeof(out->meteor_source_url), "https://www.imo.net/");
    out->meteor_enabled = load_int_or_default(app->db, "meteor_enabled", 1);

    load_text_or_default(app->db, "satellite_source_url", out->satellite_source_url, sizeof(out->satellite_source_url), "https://api.n2yo.com/rest/v1/satellite/");
    load_text_or_default(app->db, "satellite_api_base", out->satellite_api_base, sizeof(out->satellite_api_base), "https://api.n2yo.com/rest/v1/satellite/");
    load_text_or_default(app->db, "satellite_api_key", out->satellite_api_key, sizeof(out->satellite_api_key), "");
    out->satellite_enabled = load_int_or_default(app->db, "satellite_enabled", 1);
    out->satellite_days = load_int_or_default(app->db, "satellite_days", 1);
    out->satellite_min_elevation = load_int_or_default(app->db, "satellite_min_elevation", 20);
    load_text_or_default(app->db, "satellite_window_start", out->satellite_window_start, sizeof(out->satellite_window_start), "08:00");
    load_text_or_default(app->db, "satellite_window_end", out->satellite_window_end, sizeof(out->satellite_window_end), "19:00");
    load_text_or_default(app->db, "satellite_mode_filter", out->satellite_mode_filter, sizeof(out->satellite_mode_filter), "全部");
    out->satellite_max_items = load_int_or_default(app->db, "satellite_max_items", 8);

    load_text_or_default(app->db, "hamqsl_widget_url", out->hamqsl_widget_url, sizeof(out->hamqsl_widget_url), "https://www.hamqsl.com/solar101sc.php");
    load_text_or_default(app->db, "hamqsl_selected_fields", out->hamqsl_selected_fields, sizeof(out->hamqsl_selected_fields), "");
    out->include_source_urls = load_int_or_default(app->db, "include_source_urls", 1);
    out->include_hamqsl_widget = load_int_or_default(app->db, "include_hamqsl_widget", 1);

    load_text_or_default(app->db, "report_template_full", out->report_template_full, sizeof(out->report_template_full), "{{section_hamqsl}}");
    load_text_or_default(app->db, "report_template_6m", out->report_template_6m, sizeof(out->report_template_6m), "{{section_6m}}");
    load_text_or_default(app->db, "report_template_solar", out->report_template_solar, sizeof(out->report_template_solar), "{{section_solar}}");
    load_text_or_default(app->db, "report_template_geomag", out->report_template_geomag, sizeof(out->report_template_geomag), "{{section_solar}}");
    load_text_or_default(app->db, "report_template_open6m", out->report_template_open6m, sizeof(out->report_template_open6m), "{{section_6m}}");
    load_text_or_default(app->db, "help_template", out->help_template, sizeof(out->help_template), "帮助");

    load_text_or_default(app->db, "trigger_full", out->trigger_full, sizeof(out->trigger_full), "传播");
    load_text_or_default(app->db, "trigger_6m", out->trigger_6m, sizeof(out->trigger_6m), "6m,6米");
    load_text_or_default(app->db, "trigger_solar", out->trigger_solar, sizeof(out->trigger_solar), "太阳");
    load_text_or_default(app->db, "trigger_help", out->trigger_help, sizeof(out->trigger_help), "帮助");

    pthread_mutex_unlock(&app->db_mutex);

    out->http_port = clamp_int(out->http_port, 1, 65535);
    out->onebot_send_delay_ms = clamp_int(out->onebot_send_delay_ms, 0, 15000);
    out->onebot_retry_count = clamp_int(out->onebot_retry_count, 0, 5);
    out->onebot_retry_delay_ms = clamp_int(out->onebot_retry_delay_ms, 0, 30000);
    out->hamqsl_interval_minutes = clamp_int(out->hamqsl_interval_minutes, 5, 1440);
    out->weather_interval_minutes = clamp_int(out->weather_interval_minutes, 5, 1440);
    out->tropo_interval_minutes = clamp_int(out->tropo_interval_minutes, 5, 1440);
    out->meteor_interval_hours = clamp_int(out->meteor_interval_hours, 1, 168);
    out->satellite_interval_hours = clamp_int(out->satellite_interval_hours, 1, 168);
    out->psk_eval_interval_seconds = clamp_int(out->psk_eval_interval_seconds, 15, 3600);
    out->snapshot_rebuild_seconds = clamp_int(out->snapshot_rebuild_seconds, 15, 3600);
    out->psk_radius_km = clamp_int(out->psk_radius_km, 50, 3000);
    out->psk_window_minutes = clamp_int(out->psk_window_minutes, 15, 720);
    out->rate_limit_per_minute = clamp_int(out->rate_limit_per_minute, 0, 60);
    out->geomag_alert_threshold_g = clamp_int(out->geomag_alert_threshold_g, 1, 5);
    out->sixm_alert_interval_minutes = clamp_int(out->sixm_alert_interval_minutes, 1, 720);
    out->sixm_psk_trigger_spots = clamp_int(out->sixm_psk_trigger_spots, 1, 20);
    out->tropo_forecast_hours = clamp_int(out->tropo_forecast_hours, 3, 192);
    out->satellite_days = clamp_int(out->satellite_days, 1, 10);
    out->satellite_min_elevation = clamp_int(out->satellite_min_elevation, 1, 89);
    out->satellite_max_items = clamp_int(out->satellite_max_items, 1, MAX_PASSES);
    out->refresh_interval_minutes = clamp_int(out->refresh_interval_minutes, 1, 1440);
    return 0;
}

int storage_save_setting(app_t *app, const char *key, const char *value) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO settings(key, value) VALUES(?, ?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_load_targets(app_t *app, target_t *targets, int max_targets, int *out_count) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, label, type, target_id, enabled, command_enabled, notes "
                      "FROM targets ORDER BY id ASC";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    int count = 0;
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_targets) {
            target_t *t = &targets[count++];
            memset(t, 0, sizeof(*t));
            t->id = sqlite3_column_int(stmt, 0);
            copy_string(t->label, sizeof(t->label), (const char *)sqlite3_column_text(stmt, 1));
            t->type = strcmp((const char *)sqlite3_column_text(stmt, 2), "private") == 0 ? TARGET_PRIVATE : TARGET_GROUP;
            copy_string(t->target_id, sizeof(t->target_id), (const char *)sqlite3_column_text(stmt, 3));
            t->enabled = sqlite3_column_int(stmt, 4);
            t->command_enabled = sqlite3_column_int(stmt, 5);
            copy_string(t->notes, sizeof(t->notes), (const char *)sqlite3_column_text(stmt, 6));
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    if (out_count) {
        *out_count = count;
    }
    return 0;
}

int storage_add_target(app_t *app, const target_t *target) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO targets(label, type, target_id, enabled, command_enabled, notes) VALUES(?, ?, ?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, target->label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, target->type == TARGET_PRIVATE ? "private" : "group", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, target->target_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, target->enabled);
        sqlite3_bind_int(stmt, 5, target->command_enabled);
        sqlite3_bind_text(stmt, 6, target->notes, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_delete_target(app_t *app, int target_id) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, "DELETE FROM targets WHERE id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, target_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_toggle_target(app_t *app, int target_id, int enabled) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, "UPDATE targets SET enabled = ? WHERE id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled);
        sqlite3_bind_int(stmt, 2, target_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_load_schedules(app_t *app, schedule_rule_t *rules, int max_rules, int *out_count) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, label, report_kind, hhmm, enabled, last_fire_date FROM schedule_rules ORDER BY hhmm ASC, id ASC";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    int count = 0;
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_rules) {
            schedule_rule_t *rule = &rules[count++];
            memset(rule, 0, sizeof(*rule));
            rule->id = sqlite3_column_int(stmt, 0);
            copy_string(rule->label, sizeof(rule->label), (const char *)sqlite3_column_text(stmt, 1));
            copy_string(rule->report_kind, sizeof(rule->report_kind), (const char *)sqlite3_column_text(stmt, 2));
            copy_string(rule->hhmm, sizeof(rule->hhmm), (const char *)sqlite3_column_text(stmt, 3));
            rule->enabled = sqlite3_column_int(stmt, 4);
            copy_string(rule->last_fire_date, sizeof(rule->last_fire_date), (const char *)sqlite3_column_text(stmt, 5));
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    if (out_count) {
        *out_count = count;
    }
    return 0;
}

int storage_add_schedule(app_t *app, const schedule_rule_t *rule) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO schedule_rules(label, report_kind, hhmm, enabled, last_fire_date) VALUES(?, ?, ?, ?, '')";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, rule->label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, rule->report_kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, rule->hhmm, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, rule->enabled);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_delete_schedule(app_t *app, int rule_id) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, "DELETE FROM schedule_rules WHERE id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, rule_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_toggle_schedule(app_t *app, int rule_id, int enabled) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, "UPDATE schedule_rules SET enabled = ? WHERE id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled);
        sqlite3_bind_int(stmt, 2, rule_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_set_schedule_last_fire(app_t *app, int rule_id, const char *date_text) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, "UPDATE schedule_rules SET last_fire_date = ? WHERE id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, date_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, rule_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_load_satellites(app_t *app, satellite_t *sats, int max_sats, int *out_count) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT id, name, norad_id, mode_type, enabled, notes FROM satellites ORDER BY enabled DESC, name ASC";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    int count = 0;
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_sats) {
            satellite_t *sat = &sats[count++];
            memset(sat, 0, sizeof(*sat));
            sat->id = sqlite3_column_int(stmt, 0);
            copy_string(sat->name, sizeof(sat->name), (const char *)sqlite3_column_text(stmt, 1));
            sat->norad_id = sqlite3_column_int(stmt, 2);
            copy_string(sat->mode_type, sizeof(sat->mode_type), (const char *)sqlite3_column_text(stmt, 3));
            sat->enabled = sqlite3_column_int(stmt, 4);
            copy_string(sat->notes, sizeof(sat->notes), (const char *)sqlite3_column_text(stmt, 5));
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    if (out_count) {
        *out_count = count;
    }
    return 0;
}

int storage_add_satellite(app_t *app, const satellite_t *sat) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO satellites(name, norad_id, mode_type, enabled, notes) VALUES(?, ?, ?, ?, ?)";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, sat->name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, sat->norad_id);
        sqlite3_bind_text(stmt, 3, sat->mode_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, sat->enabled);
        sqlite3_bind_text(stmt, 5, sat->notes, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_delete_satellite(app_t *app, int sat_id) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, "DELETE FROM satellites WHERE id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, sat_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_toggle_satellite(app_t *app, int sat_id, int enabled) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(app->db, "UPDATE satellites SET enabled = ? WHERE id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled);
        sqlite3_bind_int(stmt, 2, sat_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_get_state(app_t *app, const char *key, char *out, size_t out_len) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM state WHERE key = ?";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            copy_string(out, out_len, (const char *)sqlite3_column_text(stmt, 0));
        } else if (out && out_len > 0) {
            out[0] = '\0';
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return 0;
}

int storage_set_state(app_t *app, const char *key, const char *value) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO state(key, value) VALUES(?, ?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_load_recent_logs(app_t *app, sb_t *html_rows) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT created_at, level, message FROM logs ORDER BY id DESC LIMIT 30";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const char *created_at = (const char *)sqlite3_column_text(stmt, 0);
            const char *level = (const char *)sqlite3_column_text(stmt, 1);
            const char *message = (const char *)sqlite3_column_text(stmt, 2);
            sb_append(html_rows, "<tr><td>");
            html_escape_to_sb(html_rows, created_at ? created_at : "");
            sb_append(html_rows, "</td><td>");
            html_escape_to_sb(html_rows, level ? level : "");
            sb_append(html_rows, "</td><td>");
            html_escape_to_sb(html_rows, message ? message : "");
            sb_append(html_rows, "</td></tr>");
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return 0;
}

int storage_set_last_fire(app_t *app, const char *slot, const char *date_text) {
    int rule_id = 0;
    if (strcmp(slot, "morning") == 0) {
        rule_id = 1;
    } else if (strcmp(slot, "evening") == 0) {
        rule_id = 2;
    } else {
        return -1;
    }
    return storage_set_schedule_last_fire(app, rule_id, date_text);
}

int storage_get_last_fire(app_t *app, const char *slot, char *out, size_t out_len) {
    schedule_rule_t rules[MAX_SCHEDULES];
    int count = 0;
    storage_load_schedules(app, rules, MAX_SCHEDULES, &count);
    int rule_id = strcmp(slot, "morning") == 0 ? 1 : 2;
    for (int i = 0; i < count; ++i) {
        if (rules[i].id == rule_id) {
            copy_string(out, out_len, rules[i].last_fire_date);
            return 0;
        }
    }
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    return 0;
}
