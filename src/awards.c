#include "app.h"

typedef struct {
    char station_callsign[MAX_TEXT];
    char owncall[MAX_TEXT];
    char call[MAX_TEXT];
    char qso_date[MAX_TEXT];
    char time_on[MAX_TEXT];
    char band[MAX_SMALL_TEXT];
    char mode[MAX_TEXT];
    char mode_group[MAX_TEXT];
    char qsl_rcvd[MAX_TEXT];
    char qslrdate[MAX_TEXT];
    char rxqso[MAX_TEXT];
    char rxqsl[MAX_TEXT];
    int dxcc;
    char dxcc_status[MAX_TEXT];
    char country[MAX_TEXT];
    char continent[MAX_TEXT];
    char gridsquare[MAX_TEXT];
    char vucc_grids[MAX_HUGE_TEXT];
    char credit_granted[MAX_HUGE_TEXT];
    char credit_submitted[MAX_HUGE_TEXT];
    char qso_timestamp[MAX_TEXT];
    char source_key[MAX_HUGE_TEXT];
} lotw_record_t;

typedef struct {
    const char *key;
    const char *name;
    const char *continent;
    int threshold;
} qrz_rule_t;

static const qrz_rule_t QRZ_RULES[] = {
    {"asia_master", "亚洲硕士", "AS", 54},
    {"europe_master", "欧洲硕士", "EU", 66},
    {"africa_master", "非洲硕士", "AF", 76},
    {"north_america_master", "北美硕士", "NA", 49},
    {"oceania_master", "大洋洲硕士", "OC", 60},
    {"south_america_master", "南美硕士", "SA", 30},
};

static void uppercase_ascii_inplace_awards(char *text) {
    if (!text) {
        return;
    }
    for (unsigned char *p = (unsigned char *)text; *p; ++p) {
        if (*p >= 'a' && *p <= 'z') {
            *p = (unsigned char)(*p - 'a' + 'A');
        }
    }
}

static int text_is_yes(const char *text) {
    if (!text || !*text) {
        return 0;
    }
    return strcasecmp(text, "Y") == 0 ||
        strcasecmp(text, "YES") == 0 ||
        strcasecmp(text, "V") == 0;
}

static void normalize_date_yyyymmdd(const char *src, char *out, size_t out_len) {
    if (!src || strlen(src) < 8) {
        copy_string(out, out_len, "");
        return;
    }
    snprintf(out, out_len, "%.4s-%.2s-%.2s", src, src + 4, src + 6);
}

static void normalize_time_hhmmss(const char *src, char *out, size_t out_len) {
    char digits[16];
    size_t pos = 0;
    if (!src) {
        copy_string(out, out_len, "");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)src; *p && pos + 1 < sizeof(digits); ++p) {
        if (isdigit(*p)) {
            digits[pos++] = (char)*p;
        }
    }
    digits[pos] = '\0';
    while (pos < 6) {
        digits[pos++] = '0';
        digits[pos] = '\0';
    }
    if (pos < 4) {
        copy_string(out, out_len, "");
        return;
    }
    snprintf(out, out_len, "%.2s:%.2s:%.2s", digits, digits + 2, digits + 4);
}

static void build_qso_timestamp(const char *qso_date, const char *time_on, char *out, size_t out_len) {
    char date_text[32];
    char time_text[32];
    normalize_date_yyyymmdd(qso_date, date_text, sizeof(date_text));
    normalize_time_hhmmss(time_on, time_text, sizeof(time_text));
    if (date_text[0] && time_text[0]) {
        snprintf(out, out_len, "%s %s", date_text, time_text);
    } else if (date_text[0]) {
        copy_string(out, out_len, date_text);
    } else {
        copy_string(out, out_len, "");
    }
}

static void normalize_band_key(const char *input, char *db_band, size_t db_band_len, char *label, size_t label_len) {
    char temp[MAX_TEXT];
    copy_string(temp, sizeof(temp), input ? input : "");
    trim_whitespace(temp);
    uppercase_ascii_inplace_awards(temp);

    if (strcmp(temp, "50") == 0 || strcmp(temp, "50MHZ") == 0 || strcmp(temp, "6M") == 0 || strcmp(temp, "6") == 0) {
        copy_string(db_band, db_band_len, "6M");
        copy_string(label, label_len, "50 MHz / 6m");
        return;
    }
    if (strcmp(temp, "144") == 0 || strcmp(temp, "144MHZ") == 0 || strcmp(temp, "2M") == 0 || strcmp(temp, "2") == 0) {
        copy_string(db_band, db_band_len, "2M");
        copy_string(label, label_len, "144 MHz / 2m");
        return;
    }
    copy_string(db_band, db_band_len, temp[0] ? temp : "6M");
    copy_string(label, label_len, temp[0] ? temp : "50 MHz / 6m");
}

static void normalize_grid4(const char *input, char *out, size_t out_len) {
    char temp[MAX_TEXT];
    size_t pos = 0;
    if (!input) {
        copy_string(out, out_len, "");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)input; *p && pos + 1 < sizeof(temp); ++p) {
        if (isalnum(*p)) {
            temp[pos++] = (char)*p;
        }
    }
    temp[pos] = '\0';
    uppercase_ascii_inplace_awards(temp);
    if (strlen(temp) >= 4) {
        temp[4] = '\0';
        copy_string(out, out_len, temp);
    } else {
        copy_string(out, out_len, "");
    }
}

static int date_is_recent(const char *date_text, int recent_days) {
    time_t cutoff = time(NULL) - (time_t)clamp_int(recent_days, 1, 3650) * 86400;
    char cutoff_iso[16];
    char compare[16];
    format_iso_date_local(cutoff, cutoff_iso, sizeof(cutoff_iso));
    if (!date_text || !*date_text) {
        return 0;
    }
    copy_string(compare, sizeof(compare), date_text);
    compare[10] = '\0';
    return strcmp(compare, cutoff_iso) >= 0;
}

static void lotw_default_status_error(app_t *app, const char *status, const char *error_detail) {
    char now_text[MAX_TEXT];
    format_time_local(time(NULL), now_text, sizeof(now_text));
    storage_set_state(app, "lotw_last_status", status ? status : "");
    storage_set_state(app, "lotw_last_error", error_detail ? error_detail : "");
    storage_set_state(app, "lotw_last_sync_at", now_text);
}

static int sql_prepare(app_t *app, const char *sql, sqlite3_stmt **stmt) {
    int rc;
    pthread_mutex_lock(&app->db_mutex);
    rc = sqlite3_prepare_v2(app->db, sql, -1, stmt, NULL);
    if (rc != SQLITE_OK) {
        pthread_mutex_unlock(&app->db_mutex);
    }
    return rc;
}

static void sql_finish(app_t *app, sqlite3_stmt *stmt) {
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&app->db_mutex);
}

static int sql_scalar_int(app_t *app, const char *sql, int fallback) {
    sqlite3_stmt *stmt = NULL;
    int value = fallback;
    if (sql_prepare(app, sql, &stmt) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            value = sqlite3_column_int(stmt, 0);
        }
        sql_finish(app, stmt);
    }
    return value;
}

static void adif_record_reset(lotw_record_t *rec) {
    memset(rec, 0, sizeof(*rec));
}

static int adif_next_token(const char **cursor,
                           char *name, size_t name_len,
                           char *value, size_t value_len,
                           int *is_eoh, int *is_eor) {
    const char *p;
    const char *gt;
    char tag[128];
    char *colon;
    int field_len;

    if (is_eoh) *is_eoh = 0;
    if (is_eor) *is_eor = 0;
    if (!cursor || !*cursor || !name || !value) {
        return 0;
    }

    p = strchr(*cursor, '<');
    if (!p) {
        *cursor = NULL;
        return 0;
    }
    gt = strchr(p, '>');
    if (!gt) {
        *cursor = NULL;
        return 0;
    }

    if ((size_t)(gt - p - 1) >= sizeof(tag)) {
        *cursor = gt + 1;
        return -1;
    }
    memcpy(tag, p + 1, (size_t)(gt - p - 1));
    tag[gt - p - 1] = '\0';
    trim_whitespace(tag);
    uppercase_ascii_inplace_awards(tag);

    if (strcmp(tag, "EOH") == 0) {
        if (is_eoh) *is_eoh = 1;
        *cursor = gt + 1;
        name[0] = '\0';
        value[0] = '\0';
        return 1;
    }
    if (strcmp(tag, "EOR") == 0) {
        if (is_eor) *is_eor = 1;
        *cursor = gt + 1;
        name[0] = '\0';
        value[0] = '\0';
        return 1;
    }

    colon = strchr(tag, ':');
    if (!colon) {
        *cursor = gt + 1;
        return -1;
    }
    *colon = '\0';
    copy_string(name, name_len, tag);
    field_len = atoi(colon + 1);
    if (field_len < 0) {
        field_len = 0;
    }

    {
        size_t copy_len = (size_t)field_len;
        const char *data = gt + 1;
        if (copy_len >= value_len) {
            copy_len = value_len - 1;
        }
        memcpy(value, data, copy_len);
        value[copy_len] = '\0';
        *cursor = gt + 1 + field_len;
    }
    return 1;
}

static void lotw_record_apply_field(lotw_record_t *rec, const char *name, const char *value) {
    if (!rec || !name || !value) {
        return;
    }
    if (strcmp(name, "STATION_CALLSIGN") == 0) {
        copy_string(rec->station_callsign, sizeof(rec->station_callsign), value);
    } else if (strcmp(name, "APP_LOTW_OWNCALL") == 0 || strcmp(name, "OWNER_CALLSIGN") == 0) {
        copy_string(rec->owncall, sizeof(rec->owncall), value);
    } else if (strcmp(name, "CALL") == 0) {
        copy_string(rec->call, sizeof(rec->call), value);
    } else if (strcmp(name, "QSO_DATE") == 0) {
        copy_string(rec->qso_date, sizeof(rec->qso_date), value);
    } else if (strcmp(name, "TIME_ON") == 0) {
        copy_string(rec->time_on, sizeof(rec->time_on), value);
    } else if (strcmp(name, "BAND") == 0) {
        copy_string(rec->band, sizeof(rec->band), value);
        uppercase_ascii_inplace_awards(rec->band);
    } else if (strcmp(name, "MODE") == 0 || strcmp(name, "APP_LOTW_MODE") == 0) {
        copy_string(rec->mode, sizeof(rec->mode), value);
        uppercase_ascii_inplace_awards(rec->mode);
    } else if (strcmp(name, "APP_LOTW_MODEGROUP") == 0) {
        copy_string(rec->mode_group, sizeof(rec->mode_group), value);
        uppercase_ascii_inplace_awards(rec->mode_group);
    } else if (strcmp(name, "QSL_RCVD") == 0) {
        copy_string(rec->qsl_rcvd, sizeof(rec->qsl_rcvd), value);
        uppercase_ascii_inplace_awards(rec->qsl_rcvd);
    } else if (strcmp(name, "QSLRDATE") == 0) {
        normalize_date_yyyymmdd(value, rec->qslrdate, sizeof(rec->qslrdate));
    } else if (strcmp(name, "APP_LOTW_RXQSO") == 0) {
        copy_string(rec->rxqso, sizeof(rec->rxqso), value);
    } else if (strcmp(name, "APP_LOTW_RXQSL") == 0) {
        copy_string(rec->rxqsl, sizeof(rec->rxqsl), value);
    } else if (strcmp(name, "DXCC") == 0) {
        rec->dxcc = atoi(value);
    } else if (strcmp(name, "APP_LOTW_DXCC_ENTITY_STATUS") == 0) {
        copy_string(rec->dxcc_status, sizeof(rec->dxcc_status), value);
    } else if (strcmp(name, "COUNTRY") == 0) {
        copy_string(rec->country, sizeof(rec->country), value);
    } else if (strcmp(name, "CONT") == 0) {
        copy_string(rec->continent, sizeof(rec->continent), value);
        uppercase_ascii_inplace_awards(rec->continent);
    } else if (strcmp(name, "GRIDSQUARE") == 0) {
        copy_string(rec->gridsquare, sizeof(rec->gridsquare), value);
    } else if (strcmp(name, "VUCC_GRIDS") == 0) {
        copy_string(rec->vucc_grids, sizeof(rec->vucc_grids), value);
    } else if (strcmp(name, "CREDIT_GRANTED") == 0 || strcmp(name, "APP_LOTW_CREDIT_GRANTED") == 0) {
        copy_string(rec->credit_granted, sizeof(rec->credit_granted), value);
    } else if (strcmp(name, "CREDIT_SUBMITTED") == 0 || strcmp(name, "APP_LOTW_CREDIT_SUBMITTED") == 0) {
        copy_string(rec->credit_submitted, sizeof(rec->credit_submitted), value);
    }
}

static int adif_next_record(const char **cursor, lotw_record_t *rec) {
    char name[128];
    char value[MAX_HUGE_TEXT];
    int is_eoh = 0;
    int is_eor = 0;
    int saw_field = 0;
    int rc;

    if (!cursor || !*cursor || !rec) {
        return 0;
    }
    adif_record_reset(rec);
    while ((rc = adif_next_token(cursor, name, sizeof(name), value, sizeof(value), &is_eoh, &is_eor)) > 0) {
        if (is_eoh) {
            continue;
        }
        if (is_eor) {
            if (saw_field) {
                build_qso_timestamp(rec->qso_date, rec->time_on, rec->qso_timestamp, sizeof(rec->qso_timestamp));
                return 1;
            }
            continue;
        }
        lotw_record_apply_field(rec, name, value);
        saw_field = 1;
    }
    if (saw_field) {
        build_qso_timestamp(rec->qso_date, rec->time_on, rec->qso_timestamp, sizeof(rec->qso_timestamp));
        return 1;
    }
    return 0;
}

static void adif_extract_header_value(const char *adif, const char *field_name, char *out, size_t out_len) {
    const char *cursor;
    char name[128];
    char value[MAX_HUGE_TEXT];
    int is_eoh = 0;
    int is_eor = 0;

    copy_string(out, out_len, "");
    if (!adif || !field_name) {
        return;
    }
    cursor = adif;
    while (adif_next_token(&cursor, name, sizeof(name), value, sizeof(value), &is_eoh, &is_eor) > 0) {
        if (is_eoh || is_eor) {
            break;
        }
        if (strcmp(name, field_name) == 0) {
            copy_string(out, out_len, value);
            return;
        }
    }
}

static void lotw_build_source_key(lotw_record_t *rec) {
    char station[MAX_TEXT];
    char call[MAX_TEXT];
    char band[MAX_TEXT];
    char mode_key[MAX_TEXT];

    copy_string(station, sizeof(station), rec->station_callsign[0] ? rec->station_callsign : rec->owncall);
    copy_string(call, sizeof(call), rec->call);
    copy_string(band, sizeof(band), rec->band);
    copy_string(mode_key, sizeof(mode_key), rec->mode_group[0] ? rec->mode_group : rec->mode);
    uppercase_ascii_inplace_awards(station);
    uppercase_ascii_inplace_awards(call);
    uppercase_ascii_inplace_awards(band);
    uppercase_ascii_inplace_awards(mode_key);
    snprintf(rec->source_key, sizeof(rec->source_key), "%s|%s|%s|%s|%s|%s",
        station, call, rec->qso_date, rec->time_on, band, mode_key);
}

static int db_upsert_lotw_record(app_t *app, const lotw_record_t *rec, const char *stamp) {
    static const char *sql =
        "INSERT INTO logbook_qsos("
        " source, source_key, station_callsign, owncall, call, qso_date, time_on, band, mode, mode_group,"
        " qso_timestamp, rxqso, qsl_rcvd, qslrdate, rxqsl, dxcc, dxcc_status, country, continent,"
        " gridsquare, vucc_grids, credit_granted, credit_submitted, raw_adif, imported_at, updated_at"
        ") VALUES("
        " 'lotw', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, '', ?, ?"
        ") ON CONFLICT(source_key) DO UPDATE SET"
        " station_callsign=CASE WHEN excluded.station_callsign<>'' THEN excluded.station_callsign ELSE logbook_qsos.station_callsign END,"
        " owncall=CASE WHEN excluded.owncall<>'' THEN excluded.owncall ELSE logbook_qsos.owncall END,"
        " call=CASE WHEN excluded.call<>'' THEN excluded.call ELSE logbook_qsos.call END,"
        " qso_date=CASE WHEN excluded.qso_date<>'' THEN excluded.qso_date ELSE logbook_qsos.qso_date END,"
        " time_on=CASE WHEN excluded.time_on<>'' THEN excluded.time_on ELSE logbook_qsos.time_on END,"
        " band=CASE WHEN excluded.band<>'' THEN excluded.band ELSE logbook_qsos.band END,"
        " mode=CASE WHEN excluded.mode<>'' THEN excluded.mode ELSE logbook_qsos.mode END,"
        " mode_group=CASE WHEN excluded.mode_group<>'' THEN excluded.mode_group ELSE logbook_qsos.mode_group END,"
        " qso_timestamp=CASE WHEN excluded.qso_timestamp<>'' THEN excluded.qso_timestamp ELSE logbook_qsos.qso_timestamp END,"
        " rxqso=CASE WHEN excluded.rxqso<>'' THEN excluded.rxqso ELSE logbook_qsos.rxqso END,"
        " qsl_rcvd=CASE WHEN excluded.qsl_rcvd<>'' THEN excluded.qsl_rcvd ELSE logbook_qsos.qsl_rcvd END,"
        " qslrdate=CASE WHEN excluded.qslrdate<>'' THEN excluded.qslrdate ELSE logbook_qsos.qslrdate END,"
        " rxqsl=CASE WHEN excluded.rxqsl<>'' THEN excluded.rxqsl ELSE logbook_qsos.rxqsl END,"
        " dxcc=CASE WHEN excluded.dxcc>0 THEN excluded.dxcc ELSE logbook_qsos.dxcc END,"
        " dxcc_status=CASE WHEN excluded.dxcc_status<>'' THEN excluded.dxcc_status ELSE logbook_qsos.dxcc_status END,"
        " country=CASE WHEN excluded.country<>'' THEN excluded.country ELSE logbook_qsos.country END,"
        " continent=CASE WHEN excluded.continent<>'' THEN excluded.continent ELSE logbook_qsos.continent END,"
        " gridsquare=CASE WHEN excluded.gridsquare<>'' THEN excluded.gridsquare ELSE logbook_qsos.gridsquare END,"
        " vucc_grids=CASE WHEN excluded.vucc_grids<>'' THEN excluded.vucc_grids ELSE logbook_qsos.vucc_grids END,"
        " credit_granted=CASE WHEN excluded.credit_granted<>'' THEN excluded.credit_granted ELSE logbook_qsos.credit_granted END,"
        " credit_submitted=CASE WHEN excluded.credit_submitted<>'' THEN excluded.credit_submitted ELSE logbook_qsos.credit_submitted END,"
        " updated_at=excluded.updated_at";
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (!rec || !rec->source_key[0]) {
        return -1;
    }
    if (sql_prepare(app, sql, &stmt) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, rec->source_key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rec->station_callsign, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, rec->owncall, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rec->call, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, rec->qso_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, rec->time_on, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, rec->band, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, rec->mode, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, rec->mode_group, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, rec->qso_timestamp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, rec->rxqso, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, rec->qsl_rcvd, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, rec->qslrdate, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 14, rec->rxqsl, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 15, rec->dxcc);
    sqlite3_bind_text(stmt, 16, rec->dxcc_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 17, rec->country, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 18, rec->continent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 19, rec->gridsquare, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 20, rec->vucc_grids, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 21, rec->credit_granted, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 22, rec->credit_submitted, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 23, stamp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 24, stamp, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sql_finish(app, stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

static char *url_escape_dup(CURL *curl, const char *input) {
    char *escaped;
    char *out;
    if (!curl || !input) {
        return NULL;
    }
    escaped = curl_easy_escape(curl, input, 0);
    if (!escaped) {
        return NULL;
    }
    out = malloc(strlen(escaped) + 1);
    if (out) {
        memcpy(out, escaped, strlen(escaped) + 1);
    }
    curl_free(escaped);
    return out;
}

static int lotw_fetch_and_store(app_t *app, const settings_t *settings, int qsl_mode,
                                const char *since_value, int *out_rows,
                                char *out_cursor, size_t out_cursor_len,
                                char *out_error, size_t out_error_len) {
    CURL *curl = NULL;
    char *login = NULL;
    char *password = NULL;
    char *owncall = NULL;
    char *since = NULL;
    char url[4096];
    char cursor_value[MAX_TEXT];
    long status = 0;
    char *body = NULL;
    const char *scan = NULL;
    int rows = 0;
    lotw_record_t rec;
    char stamp[MAX_TEXT];

    if (out_rows) {
        *out_rows = 0;
    }
    copy_string(out_cursor, out_cursor_len, "");
    copy_string(out_error, out_error_len, "");

    curl = curl_easy_init();
    if (!curl) {
        copy_string(out_error, out_error_len, "无法初始化 LoTW URL 编码器");
        return -1;
    }
    login = url_escape_dup(curl, settings->lotw_login);
    password = url_escape_dup(curl, settings->lotw_password);
    owncall = settings->lotw_station_callsign[0] ? url_escape_dup(curl, settings->lotw_station_callsign) : NULL;
    since = url_escape_dup(curl, (since_value && *since_value) ? since_value : "2000-01-01");
    if (!login || !password || !since) {
        copy_string(out_error, out_error_len, "LoTW 参数编码失败");
        curl_easy_cleanup(curl);
        free(login);
        free(password);
        free(owncall);
        free(since);
        return -1;
    }

    snprintf(url, sizeof(url),
        "https://lotw.arrl.org/lotwuser/lotwreport.adi?login=%s&password=%s&qso_query=1&qso_qsl=%s&qso_withown=yes&qso_mydetail=yes%s%s%s%s%s",
        login,
        password,
        qsl_mode ? "yes" : "no",
        qsl_mode ? "&qso_qsldetail=yes" : "",
        owncall ? "&qso_owncall=" : "",
        owncall ? owncall : "",
        qsl_mode ? "&qso_qslsince=" : "&qso_qsorxsince=",
        since);

    body = http_get_text(url, NULL, &status);
    curl_easy_cleanup(curl);
    free(login);
    free(password);
    free(owncall);
    free(since);

    if (!body || status < 200 || status >= 300) {
        snprintf(out_error, out_error_len, "LoTW 请求失败，HTTP=%ld", status);
        free(body);
        return -1;
    }
    if (!strstr(body, "<EOH>") && !strstr(body, "<eoh>")) {
        copy_string(out_error, out_error_len, "LoTW 返回内容不是 ADIF");
        free(body);
        return -1;
    }

    adif_extract_header_value(body, qsl_mode ? "APP_LOTW_LASTQSL" : "APP_LOTW_LASTQSORX", cursor_value, sizeof(cursor_value));
    if (cursor_value[0]) {
        copy_string(out_cursor, out_cursor_len, cursor_value);
    }

    scan = body;
    format_time_local(time(NULL), stamp, sizeof(stamp));
    while (adif_next_record(&scan, &rec)) {
        if (!rec.call[0] || !rec.qso_date[0]) {
            continue;
        }
        if (!rec.station_callsign[0] && settings->lotw_station_callsign[0]) {
            copy_string(rec.station_callsign, sizeof(rec.station_callsign), settings->lotw_station_callsign);
        }
        if (!rec.owncall[0] && settings->lotw_station_callsign[0]) {
            copy_string(rec.owncall, sizeof(rec.owncall), settings->lotw_station_callsign);
        }
        if (qsl_mode && !rec.qsl_rcvd[0]) {
            copy_string(rec.qsl_rcvd, sizeof(rec.qsl_rcvd), "Y");
        }
        lotw_build_source_key(&rec);
        if (db_upsert_lotw_record(app, &rec, stamp) == 0) {
            rows++;
        }
    }
    free(body);
    if (out_rows) {
        *out_rows = rows;
    }
    return 0;
}

static int qrz_rule_from_name(const char *input, qrz_award_summary_t *out) {
    char temp[MAX_TEXT];
    copy_string(temp, sizeof(temp), input ? input : "");
    trim_whitespace(temp);

    if (temp[0] == '\0' || string_contains_ci(temp, "亚洲")) {
        copy_string(out->award_key, sizeof(out->award_key), QRZ_RULES[0].key);
        copy_string(out->award_name, sizeof(out->award_name), QRZ_RULES[0].name);
        copy_string(out->continent, sizeof(out->continent), QRZ_RULES[0].continent);
        out->threshold = QRZ_RULES[0].threshold;
        return 0;
    }
    for (size_t i = 0; i < sizeof(QRZ_RULES) / sizeof(QRZ_RULES[0]); ++i) {
        if (string_contains_ci(temp, QRZ_RULES[i].name) ||
            (strstr(QRZ_RULES[i].key, "europe") && string_contains_ci(temp, "欧洲")) ||
            (strstr(QRZ_RULES[i].key, "africa") && string_contains_ci(temp, "非洲")) ||
            (strstr(QRZ_RULES[i].key, "north_america") && string_contains_ci(temp, "北美")) ||
            (strstr(QRZ_RULES[i].key, "oceania") && string_contains_ci(temp, "大洋洲")) ||
            (strstr(QRZ_RULES[i].key, "south_america") && string_contains_ci(temp, "南美"))) {
            copy_string(out->award_key, sizeof(out->award_key), QRZ_RULES[i].key);
            copy_string(out->award_name, sizeof(out->award_name), QRZ_RULES[i].name);
            copy_string(out->continent, sizeof(out->continent), QRZ_RULES[i].continent);
            out->threshold = QRZ_RULES[i].threshold;
            return 0;
        }
    }
    return -1;
}

static int append_unique_text(char values[][64], int *count, int max_count, const char *value) {
    for (int i = 0; i < *count; ++i) {
        if (strcmp(values[i], value) == 0) {
            return 0;
        }
    }
    if (*count >= max_count) {
        return -1;
    }
    copy_string(values[*count], 64, value);
    (*count)++;
    return 0;
}

static void collect_vucc_grids_from_text(char grids[][64], int *count, int max_count, const char *text) {
    char temp[MAX_HUGE_TEXT];
    char *save = NULL;
    copy_string(temp, sizeof(temp), text ? text : "");
    for (char *part = strtok_r(temp, ",;/ \t\r\n", &save); part; part = strtok_r(NULL, ",;/ \t\r\n", &save)) {
        char grid4[8];
        normalize_grid4(part, grid4, sizeof(grid4));
        if (grid4[0]) {
            append_unique_text(grids, count, max_count, grid4);
        }
    }
}

static int message_matches_csv_local(const char *message, const char *csv) {
    char temp[MAX_HUGE_TEXT];
    char *save = NULL;
    if (!message || !csv || !*csv) {
        return 0;
    }
    copy_string(temp, sizeof(temp), csv);
    for (char *part = strtok_r(temp, ",|/ \t\r\n", &save); part; part = strtok_r(NULL, ",|/ \t\r\n", &save)) {
        trim_whitespace(part);
        if (*part && string_contains_ci(message, part)) {
            return 1;
        }
    }
    return 0;
}

int awards_sync_lotw(app_t *app, int force_full) {
    settings_t settings;
    char qso_cursor[MAX_TEXT] = {0};
    char qsl_cursor[MAX_TEXT] = {0};
    char next_qso_cursor[MAX_TEXT] = {0};
    char next_qsl_cursor[MAX_TEXT] = {0};
    char error_text[MAX_LARGE_TEXT] = {0};
    char status_text[MAX_LARGE_TEXT] = {0};
    char sync_at[MAX_TEXT];
    int qso_rows = 0;
    int qsl_rows = 0;
    int rc_qso = 0;
    int rc_qsl = 0;

    storage_load_settings(app, &settings);
    if (!settings.lotw_enabled) {
        lotw_default_status_error(app, "LoTW 未启用", "");
        return 0;
    }
    if (!settings.lotw_login[0] || !settings.lotw_password[0]) {
        lotw_default_status_error(app, "LoTW 未配置", "请先填写 LoTW 登录名和密码。");
        return -1;
    }

    if (!force_full) {
        storage_get_state(app, "lotw_last_qso_cursor", qso_cursor, sizeof(qso_cursor));
        storage_get_state(app, "lotw_last_qsl_cursor", qsl_cursor, sizeof(qsl_cursor));
    }
    if (settings.lotw_fetch_qso_enabled) {
        rc_qso = lotw_fetch_and_store(app, &settings, 0, force_full ? "2000-01-01" : qso_cursor,
            &qso_rows, next_qso_cursor, sizeof(next_qso_cursor), error_text, sizeof(error_text));
        if (rc_qso != 0) {
            lotw_default_status_error(app, "LoTW 同步失败", error_text);
            app_log(app, "WARN", "LoTW QSO 同步失败: %s", error_text);
            return -1;
        }
    }
    if (settings.lotw_fetch_qsl_enabled) {
        rc_qsl = lotw_fetch_and_store(app, &settings, 1, force_full ? "2000-01-01" : qsl_cursor,
            &qsl_rows, next_qsl_cursor, sizeof(next_qsl_cursor), error_text, sizeof(error_text));
        if (rc_qsl != 0) {
            lotw_default_status_error(app, "LoTW 同步失败", error_text);
            app_log(app, "WARN", "LoTW QSL 同步失败: %s", error_text);
            return -1;
        }
    }

    if (next_qso_cursor[0]) {
        storage_set_state(app, "lotw_last_qso_cursor", next_qso_cursor);
    }
    if (next_qsl_cursor[0]) {
        storage_set_state(app, "lotw_last_qsl_cursor", next_qsl_cursor);
    }
    format_time_local(time(NULL), sync_at, sizeof(sync_at));
    snprintf(status_text, sizeof(status_text), "同步完成: QSO %d 条 / QSL %d 条", qso_rows, qsl_rows);
    storage_set_state(app, "lotw_last_status", status_text);
    storage_set_state(app, "lotw_last_error", "");
    storage_set_state(app, "lotw_last_sync_at", sync_at);
    app_log(app, "INFO", "LoTW 同步完成: qso=%d qsl=%d force=%d", qso_rows, qsl_rows, force_full ? 1 : 0);
    return 0;
}

int awards_load_lotw_status(app_t *app, lotw_status_t *out) {
    settings_t settings;
    memset(out, 0, sizeof(*out));
    storage_load_settings(app, &settings);
    out->configured = settings.lotw_login[0] && settings.lotw_password[0];
    out->enabled = settings.lotw_enabled;
    copy_string(out->callsign, sizeof(out->callsign),
        settings.lotw_station_callsign[0] ? settings.lotw_station_callsign :
        (settings.station_name[0] ? settings.station_name : ""));
    storage_get_state(app, "lotw_last_sync_at", out->last_sync_at, sizeof(out->last_sync_at));
    storage_get_state(app, "lotw_last_qso_cursor", out->last_qso_cursor, sizeof(out->last_qso_cursor));
    storage_get_state(app, "lotw_last_qsl_cursor", out->last_qsl_cursor, sizeof(out->last_qsl_cursor));
    storage_get_state(app, "lotw_last_status", out->last_status, sizeof(out->last_status));
    storage_get_state(app, "lotw_last_error", out->last_error, sizeof(out->last_error));
    out->total_qsos = sql_scalar_int(app, "SELECT COUNT(*) FROM logbook_qsos", 0);
    out->confirmed_qsos = sql_scalar_int(app, "SELECT COUNT(*) FROM logbook_qsos WHERE UPPER(qsl_rcvd)='Y'", 0);
    return 0;
}

int awards_load_dxcc_summary(app_t *app, dxcc_summary_t *out) {
    settings_t settings;
    char recent_sql[512];
    memset(out, 0, sizeof(*out));
    storage_load_settings(app, &settings);
    out->confirmed_current = sql_scalar_int(app,
        "SELECT COUNT(DISTINCT dxcc) FROM logbook_qsos WHERE dxcc>0 AND UPPER(qsl_rcvd)='Y' "
        "AND UPPER(COALESCE(dxcc_status,'')) NOT LIKE 'DELETED%'", 0);
    out->confirmed_deleted = sql_scalar_int(app,
        "SELECT COUNT(DISTINCT dxcc) FROM logbook_qsos WHERE dxcc>0 AND UPPER(qsl_rcvd)='Y' "
        "AND UPPER(COALESCE(dxcc_status,'')) LIKE 'DELETED%'", 0);
    out->confirmed_total = out->confirmed_current + out->confirmed_deleted;
    out->unconfirmed_qsos = sql_scalar_int(app,
        "SELECT COUNT(*) FROM logbook_qsos WHERE UPPER(COALESCE(qsl_rcvd,''))<>'Y'", 0);
    out->granted_credits = sql_scalar_int(app,
        "SELECT COUNT(DISTINCT dxcc) FROM logbook_qsos WHERE dxcc>0 AND UPPER(qsl_rcvd)='Y' "
        "AND UPPER(COALESCE(credit_granted,'')) LIKE '%DXCC%'", 0);
    snprintf(recent_sql, sizeof(recent_sql),
        "SELECT COUNT(DISTINCT dxcc) FROM logbook_qsos WHERE dxcc>0 AND UPPER(qsl_rcvd)='Y' "
        "AND substr(COALESCE(NULLIF(rxqsl,''), NULLIF(qslrdate,''), updated_at),1,10) >= date('now','-%d day')",
        settings.award_recent_days);
    out->recent_confirmed = sql_scalar_int(app, recent_sql, 0);
    return 0;
}

int awards_load_vucc_summary(app_t *app, const char *band_key, vucc_summary_t *out) {
    sqlite3_stmt *stmt = NULL;
    char db_band[MAX_SMALL_TEXT];
    char band_label[MAX_TEXT];
    char recent_grids[4096][64];
    char all_grids[4096][64];
    int recent_count = 0;
    int all_count = 0;
    settings_t settings;

    memset(out, 0, sizeof(*out));
    normalize_band_key(band_key, db_band, sizeof(db_band), band_label, sizeof(band_label));
    copy_string(out->band_key, sizeof(out->band_key), db_band);
    copy_string(out->band_label, sizeof(out->band_label), band_label);
    out->threshold_basic = 100;
    storage_load_settings(app, &settings);

    if (sql_prepare(app,
            "SELECT qsl_rcvd, gridsquare, vucc_grids, qslrdate, rxqsl, updated_at FROM logbook_qsos WHERE UPPER(band)=?",
            &stmt) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, db_band, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *qsl_rcvd = (const char *)sqlite3_column_text(stmt, 0);
        const char *gridsquare = (const char *)sqlite3_column_text(stmt, 1);
        const char *vucc_grids = (const char *)sqlite3_column_text(stmt, 2);
        const char *qslrdate = (const char *)sqlite3_column_text(stmt, 3);
        const char *rxqsl = (const char *)sqlite3_column_text(stmt, 4);
        const char *updated_at = (const char *)sqlite3_column_text(stmt, 5);
        char recent_date[MAX_TEXT];

        if (text_is_yes(qsl_rcvd)) {
            collect_vucc_grids_from_text(all_grids, &all_count, 4096, vucc_grids && *vucc_grids ? vucc_grids : gridsquare);
            copy_string(recent_date, sizeof(recent_date),
                (rxqsl && *rxqsl) ? rxqsl : ((qslrdate && *qslrdate) ? qslrdate : (updated_at ? updated_at : "")));
            if (date_is_recent(recent_date, settings.award_recent_days)) {
                collect_vucc_grids_from_text(recent_grids, &recent_count, 4096, vucc_grids && *vucc_grids ? vucc_grids : gridsquare);
            }
        } else {
            out->unconfirmed_qsos++;
        }
    }
    sql_finish(app, stmt);

    out->confirmed_grids = all_count;
    out->recent_grids = recent_count;
    out->remaining_to_basic = out->confirmed_grids >= out->threshold_basic ? 0 : (out->threshold_basic - out->confirmed_grids);
    return 0;
}

int awards_load_qrz_summary(app_t *app, const char *award_name, qrz_award_summary_t *out) {
    sqlite3_stmt *stmt = NULL;
    settings_t settings;
    char recent_date[MAX_TEXT];
    char recent_pairs[4096][64];
    char all_pairs[4096][64];
    int recent_count = 0;
    int all_count = 0;

    memset(out, 0, sizeof(*out));
    if (qrz_rule_from_name(award_name, out) != 0) {
        return -1;
    }
    storage_load_settings(app, &settings);

    if (sql_prepare(app,
            "SELECT dxcc, band, qslrdate, rxqsl, updated_at FROM logbook_qsos "
            "WHERE dxcc>0 AND UPPER(qsl_rcvd)='Y' AND UPPER(continent)=?",
            &stmt) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, out->continent, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int dxcc = sqlite3_column_int(stmt, 0);
        const char *band = (const char *)sqlite3_column_text(stmt, 1);
        const char *qslrdate = (const char *)sqlite3_column_text(stmt, 2);
        const char *rxqsl = (const char *)sqlite3_column_text(stmt, 3);
        const char *updated_at = (const char *)sqlite3_column_text(stmt, 4);
        char pair[64];
        snprintf(pair, sizeof(pair), "%d|%s", dxcc, band ? band : "");
        append_unique_text(all_pairs, &all_count, 4096, pair);
        copy_string(recent_date, sizeof(recent_date),
            (rxqsl && *rxqsl) ? rxqsl : ((qslrdate && *qslrdate) ? qslrdate : (updated_at ? updated_at : "")));
        if (date_is_recent(recent_date, settings.award_recent_days)) {
            append_unique_text(recent_pairs, &recent_count, 4096, pair);
        }
    }
    sql_finish(app, stmt);

    out->credits = all_count;
    out->recent_credits = recent_count;
    out->remaining = out->credits >= out->threshold ? 0 : (out->threshold - out->credits);
    out->achieved = out->remaining == 0;
    return 0;
}

static int message_extract_vucc_band(const char *message, char *band_key, size_t band_key_len) {
    char upper[512];
    const char *vucc;
    copy_string(upper, sizeof(upper), message ? message : "");
    uppercase_ascii_inplace_awards(upper);
    vucc = strstr(upper, "VUCC");
    if (vucc) {
        const char *p = vucc;
        while (p > upper && isdigit((unsigned char)p[-1])) {
            p--;
        }
        if (p < vucc) {
            size_t len = (size_t)(vucc - p);
            if (len >= band_key_len) {
                len = band_key_len - 1;
            }
            memcpy(band_key, p, len);
            band_key[len] = '\0';
            return 0;
        }
    }
    if (string_contains_ci(message, "144") || string_contains_ci(message, "2m")) {
        copy_string(band_key, band_key_len, "144");
        return 0;
    }
    copy_string(band_key, band_key_len, "50");
    return 0;
}

static void render_dxcc_report(const settings_t *settings, const lotw_status_t *lotw,
                               const dxcc_summary_t *dxcc, char *out, size_t out_len) {
    char recent_days[16];
    char granted[16];
    char confirmed_total[16];
    char confirmed_current[16];
    char confirmed_deleted[16];
    char recent_confirmed[16];
    char unconfirmed_qsos[16];
    template_token_t tokens[] = {
        {"lotw_callsign", lotw->callsign},
        {"lotw_last_sync", lotw->last_sync_at[0] ? lotw->last_sync_at : "尚未同步"},
        {"lotw_status", lotw->last_status[0] ? lotw->last_status : "未同步"},
        {"award_recent_days", recent_days},
        {"dxcc_confirmed_total", confirmed_total},
        {"dxcc_confirmed_current", confirmed_current},
        {"dxcc_confirmed_deleted", confirmed_deleted},
        {"dxcc_granted_credits", granted},
        {"dxcc_recent_confirmed", recent_confirmed},
        {"dxcc_unconfirmed_qsos", unconfirmed_qsos},
    };
    snprintf(recent_days, sizeof(recent_days), "%d", settings->award_recent_days);
    snprintf(granted, sizeof(granted), "%d", dxcc->granted_credits);
    snprintf(confirmed_total, sizeof(confirmed_total), "%d", dxcc->confirmed_total);
    snprintf(confirmed_current, sizeof(confirmed_current), "%d", dxcc->confirmed_current);
    snprintf(confirmed_deleted, sizeof(confirmed_deleted), "%d", dxcc->confirmed_deleted);
    snprintf(recent_confirmed, sizeof(recent_confirmed), "%d", dxcc->recent_confirmed);
    snprintf(unconfirmed_qsos, sizeof(unconfirmed_qsos), "%d", dxcc->unconfirmed_qsos);
    app_render_template(out, out_len, settings->report_template_dxcc, tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void render_vucc_report(const settings_t *settings, const lotw_status_t *lotw,
                               const vucc_summary_t *vucc, char *out, size_t out_len) {
    char recent_days[16];
    char confirmed_grids[16];
    char recent_grids[16];
    char threshold[16];
    char remaining[16];
    char unconfirmed_qsos[16];
    template_token_t tokens[] = {
        {"lotw_callsign", lotw->callsign},
        {"lotw_last_sync", lotw->last_sync_at[0] ? lotw->last_sync_at : "尚未同步"},
        {"lotw_status", lotw->last_status[0] ? lotw->last_status : "未同步"},
        {"award_recent_days", recent_days},
        {"vucc_band_label", vucc->band_label},
        {"vucc_confirmed_grids", confirmed_grids},
        {"vucc_recent_grids", recent_grids},
        {"vucc_threshold_basic", threshold},
        {"vucc_remaining_to_basic", remaining},
        {"vucc_unconfirmed_qsos", unconfirmed_qsos},
    };
    snprintf(recent_days, sizeof(recent_days), "%d", settings->award_recent_days);
    snprintf(confirmed_grids, sizeof(confirmed_grids), "%d", vucc->confirmed_grids);
    snprintf(recent_grids, sizeof(recent_grids), "%d", vucc->recent_grids);
    snprintf(threshold, sizeof(threshold), "%d", vucc->threshold_basic);
    snprintf(remaining, sizeof(remaining), "%d", vucc->remaining_to_basic);
    snprintf(unconfirmed_qsos, sizeof(unconfirmed_qsos), "%d", vucc->unconfirmed_qsos);
    app_render_template(out, out_len, settings->report_template_vucc, tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void render_qrz_report(const settings_t *settings, const lotw_status_t *lotw,
                              const qrz_award_summary_t *award, char *out, size_t out_len) {
    char recent_days[16];
    char credits[16];
    char threshold[16];
    char remaining[16];
    char recent_credits[16];
    const char *status_text = award->achieved ? "已达到申请门槛" : "距离门槛仍有差距";
    template_token_t tokens[] = {
        {"lotw_callsign", lotw->callsign},
        {"lotw_last_sync", lotw->last_sync_at[0] ? lotw->last_sync_at : "尚未同步"},
        {"award_recent_days", recent_days},
        {"qrz_award_name", award->award_name},
        {"qrz_credits", credits},
        {"qrz_threshold", threshold},
        {"qrz_remaining", remaining},
        {"qrz_recent_credits", recent_credits},
        {"qrz_status", status_text},
    };
    snprintf(recent_days, sizeof(recent_days), "%d", settings->award_recent_days);
    snprintf(credits, sizeof(credits), "%d", award->credits);
    snprintf(threshold, sizeof(threshold), "%d", award->threshold);
    snprintf(remaining, sizeof(remaining), "%d", award->remaining);
    snprintf(recent_credits, sizeof(recent_credits), "%d", award->recent_credits);
    app_render_template(out, out_len, settings->report_template_qrz_award, tokens, sizeof(tokens) / sizeof(tokens[0]));
}

int awards_render_query_reply(app_t *app, const char *message, char *out, size_t out_len) {
    settings_t settings;
    lotw_status_t lotw;

    storage_load_settings(app, &settings);
    awards_load_lotw_status(app, &lotw);
    if (!lotw.configured) {
        copy_string(out, out_len, "LoTW 尚未配置，请先在后台的“日志与奖项”里填写登录信息。");
        return 0;
    }
    if (message_matches_csv_local(message, settings.trigger_dxcc) || string_contains_ci(message, "DXCC")) {
        dxcc_summary_t dxcc;
        awards_load_dxcc_summary(app, &dxcc);
        render_dxcc_report(&settings, &lotw, &dxcc, out, out_len);
        return 0;
    }
    if (message_matches_csv_local(message, settings.trigger_vucc) || string_contains_ci(message, "VUCC")) {
        char band_key[MAX_TEXT];
        vucc_summary_t vucc;
        message_extract_vucc_band(message, band_key, sizeof(band_key));
        awards_load_vucc_summary(app, band_key, &vucc);
        render_vucc_report(&settings, &lotw, &vucc, out, out_len);
        return 0;
    }
    if (message_matches_csv_local(message, settings.trigger_qrz_award) || string_contains_ci(message, "QRZ")) {
        qrz_award_summary_t award;
        const char *qrz_part = strstr(message, "QRZ");
        if (!qrz_part) {
            qrz_part = strstr(message, "qrz");
        }
        if (awards_load_qrz_summary(app, qrz_part ? qrz_part + 3 : message, &award) == 0) {
            render_qrz_report(&settings, &lotw, &award, out, out_len);
            return 0;
        }
    }
    return -1;
}

void awards_render_dashboard_html(app_t *app, sb_t *html) {
    lotw_status_t lotw;
    dxcc_summary_t dxcc;
    vucc_summary_t vucc6;
    vucc_summary_t vucc2;
    qrz_award_summary_t asia;

    awards_load_lotw_status(app, &lotw);
    awards_load_dxcc_summary(app, &dxcc);
    awards_load_vucc_summary(app, "50", &vucc6);
    awards_load_vucc_summary(app, "144", &vucc2);
    awards_load_qrz_summary(app, "亚洲硕士", &asia);

    sb_append(html, "<div class=\"grid\">");
    sb_append(html, "<section class=\"card\"><h2>日志与奖项</h2><p>");
    sb_appendf(html,
        "<span class=\"pill\">LoTW %s</span><span class=\"pill\">QSO %d</span><span class=\"pill\">已确认 %d</span>",
        lotw.enabled ? (lotw.configured ? "已配置" : "待配置") : "未启用",
        lotw.total_qsos,
        lotw.confirmed_qsos);
    sb_append(html, "</p><table>");
    sb_appendf(html, "<tr><th>最近同步</th><td>%s</td><th>状态</th><td>%s</td></tr>",
        lotw.last_sync_at[0] ? lotw.last_sync_at : "尚未同步",
        lotw.last_status[0] ? lotw.last_status : "未同步");
    sb_appendf(html, "<tr><th>QSO 游标</th><td>%s</td><th>QSL 游标</th><td>%s</td></tr>",
        lotw.last_qso_cursor[0] ? lotw.last_qso_cursor : "-",
        lotw.last_qsl_cursor[0] ? lotw.last_qsl_cursor : "-");
    sb_append(html, "</table>");
    if (lotw.last_error[0]) {
        sb_append(html, "<p class=\"help\">");
        html_escape_to_sb(html, lotw.last_error);
        sb_append(html, "</p>");
    }
    sb_append(html, "<div class=\"toolbar\" style=\"margin-top:12px;\">"
        "<form method=\"post\" action=\"/actions/lotw_sync\"><button type=\"submit\">立即同步 LoTW</button></form>"
        "</div></section>");

    sb_append(html, "<section class=\"card\"><h2>奖项概览</h2><table>");
    sb_appendf(html, "<tr><th>DXCC</th><td>已确认 %d（Current %d / Deleted %d）</td></tr>",
        dxcc.confirmed_total, dxcc.confirmed_current, dxcc.confirmed_deleted);
    sb_appendf(html, "<tr><th>50 MHz VUCC</th><td>%d 格，距离 100 格还差 %d</td></tr>",
        vucc6.confirmed_grids, vucc6.remaining_to_basic);
    sb_appendf(html, "<tr><th>144 MHz VUCC</th><td>%d 格，距离 100 格还差 %d</td></tr>",
        vucc2.confirmed_grids, vucc2.remaining_to_basic);
    sb_appendf(html, "<tr><th>QRZ 亚洲硕士</th><td>%d / %d，%s</td></tr>",
        asia.credits, asia.threshold, asia.achieved ? "已达门槛" : "未达门槛");
    sb_append(html, "</table><p class=\"help\">当前奖项统计优先使用 LoTW 已确认记录。Club Log / QRZ 账号这轮先接入后台保存，后续再扩实时同步。</p></section>");
    sb_append(html, "</div>");
}

int awards_append_template_help(sb_t *sb) {
    sb_append(sb,
        "<p class=\"help\">奖项模板变量："
        "{{lotw_callsign}} {{lotw_last_sync}} {{lotw_status}} "
        "{{dxcc_confirmed_total}} {{dxcc_confirmed_current}} {{dxcc_confirmed_deleted}} {{dxcc_granted_credits}} {{dxcc_unconfirmed_qsos}} {{dxcc_recent_confirmed}} "
        "{{vucc_band_label}} {{vucc_confirmed_grids}} {{vucc_remaining_to_basic}} {{vucc_recent_grids}} {{vucc_unconfirmed_qsos}} "
        "{{qrz_award_name}} {{qrz_credits}} {{qrz_threshold}} {{qrz_remaining}} {{qrz_recent_credits}} {{qrz_status}} "
        "{{award_recent_days}}</p>");
    return 0;
}
