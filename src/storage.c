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

static int load_setting(sqlite3 *db, const char *key, char *out, size_t out_len) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM settings WHERE key = ?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(stmt, 0);
        copy_string(out, out_len, (const char *)value);
        rc = SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_OK ? SQLITE_OK : SQLITE_NOTFOUND;
}

static int save_default_settings(sqlite3 *db) {
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
        {"latitude", "31.2304"},
        {"longitude", "121.4737"},
        {"timezone", "Asia/Shanghai"},
        {"onebot_api_base", "http://127.0.0.1:3000"},
        {"onebot_access_token", ""},
        {"onebot_webhook_token", "change-me"},
        {"schedule_morning", "08:30"},
        {"schedule_evening", "20:30"},
        {"morning_enabled", "1"},
        {"evening_enabled", "1"},
        {"refresh_interval_minutes", "15"},
        {"psk_radius_km", "800"},
        {"psk_window_minutes", "60"},
    };

    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        int rc = upsert_default(db, defaults[i].key, defaults[i].value);
        if (rc != SQLITE_OK) {
            return rc;
        }
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT OR IGNORE INTO scheduler_state(slot, last_fire_date) VALUES(?, '')";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return rc;
    }
    sqlite3_bind_text(stmt, 1, "morning", -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, "evening", -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
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
        "CREATE TABLE IF NOT EXISTS logs ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " created_at TEXT NOT NULL,"
        " level TEXT NOT NULL,"
        " message TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS scheduler_state ("
        " slot TEXT PRIMARY KEY,"
        " last_fire_date TEXT NOT NULL DEFAULT ''"
        ");");

    if (save_default_settings(app->db) != SQLITE_OK) {
        return -1;
    }
    return 0;
}

int storage_load_settings(app_t *app, settings_t *out) {
    char value[256];
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&app->db_mutex);
    load_setting(app->db, "bind_addr", out->bind_addr, sizeof(out->bind_addr));
    if (load_setting(app->db, "http_port", value, sizeof(value)) == SQLITE_OK) {
        out->http_port = atoi(value);
    }
    load_setting(app->db, "admin_user", out->admin_user, sizeof(out->admin_user));
    load_setting(app->db, "admin_password", out->admin_password, sizeof(out->admin_password));
    load_setting(app->db, "station_name", out->station_name, sizeof(out->station_name));
    load_setting(app->db, "station_grid", out->station_grid, sizeof(out->station_grid));
    if (load_setting(app->db, "latitude", value, sizeof(value)) == SQLITE_OK) {
        out->latitude = atof(value);
    }
    if (load_setting(app->db, "longitude", value, sizeof(value)) == SQLITE_OK) {
        out->longitude = atof(value);
    }
    load_setting(app->db, "timezone", out->timezone, sizeof(out->timezone));
    load_setting(app->db, "onebot_api_base", out->onebot_api_base, sizeof(out->onebot_api_base));
    load_setting(app->db, "onebot_access_token", out->onebot_access_token, sizeof(out->onebot_access_token));
    load_setting(app->db, "onebot_webhook_token", out->onebot_webhook_token, sizeof(out->onebot_webhook_token));
    load_setting(app->db, "schedule_morning", out->schedule_morning, sizeof(out->schedule_morning));
    load_setting(app->db, "schedule_evening", out->schedule_evening, sizeof(out->schedule_evening));
    if (load_setting(app->db, "morning_enabled", value, sizeof(value)) == SQLITE_OK) {
        out->morning_enabled = atoi(value);
    }
    if (load_setting(app->db, "evening_enabled", value, sizeof(value)) == SQLITE_OK) {
        out->evening_enabled = atoi(value);
    }
    if (load_setting(app->db, "refresh_interval_minutes", value, sizeof(value)) == SQLITE_OK) {
        out->refresh_interval_minutes = atoi(value);
    }
    if (load_setting(app->db, "psk_radius_km", value, sizeof(value)) == SQLITE_OK) {
        out->psk_radius_km = atoi(value);
    }
    if (load_setting(app->db, "psk_window_minutes", value, sizeof(value)) == SQLITE_OK) {
        out->psk_window_minutes = atoi(value);
    }
    pthread_mutex_unlock(&app->db_mutex);

    if (out->http_port <= 0) {
        out->http_port = 8080;
    }
    if (out->refresh_interval_minutes <= 0) {
        out->refresh_interval_minutes = 15;
    }
    if (out->psk_radius_km <= 0) {
        out->psk_radius_km = 800;
    }
    if (out->psk_window_minutes <= 0) {
        out->psk_window_minutes = 60;
    }
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
            const char *type = (const char *)sqlite3_column_text(stmt, 2);
            t->type = (type && strcmp(type, "private") == 0) ? TARGET_PRIVATE : TARGET_GROUP;
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
    const char *sql = "INSERT INTO targets(label, type, target_id, enabled, command_enabled, notes) "
                      "VALUES(?, ?, ?, ?, ?, ?)";
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
    const char *sql = "DELETE FROM targets WHERE id = ?";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
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
    const char *sql = "UPDATE targets SET enabled = ? WHERE id = ?";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, enabled);
        sqlite3_bind_int(stmt, 2, target_id);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_set_last_fire(app_t *app, const char *slot, const char *date_text) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO scheduler_state(slot, last_fire_date) VALUES(?, ?) "
                      "ON CONFLICT(slot) DO UPDATE SET last_fire_date=excluded.last_fire_date";
    int rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, slot, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, date_text, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int storage_get_last_fire(app_t *app, const char *slot, char *out, size_t out_len) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT last_fire_date FROM scheduler_state WHERE slot = ?";
    int sql_rc = sqlite3_prepare_v2(app->db, sql, -1, &stmt, NULL);
    if (sql_rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, slot, -1, SQLITE_TRANSIENT);
        sql_rc = sqlite3_step(stmt);
        if (sql_rc == SQLITE_ROW) {
            copy_string(out, out_len, (const char *)sqlite3_column_text(stmt, 0));
        } else if (out && out_len > 0) {
            out[0] = '\0';
        }
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
    return 0;
}

int storage_load_recent_logs(app_t *app, sb_t *html_rows) {
    pthread_mutex_lock(&app->db_mutex);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT created_at, level, message FROM logs ORDER BY id DESC LIMIT 20";
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
