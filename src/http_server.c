#include "app.h"

typedef struct {
    app_t *app;
    app_socket_t client_fd;
} client_ctx_t;

static int read_http_request(app_socket_t fd, http_request_t *req) {
    size_t cap = sizeof(req->body) + 4096;
    char *buffer = calloc(1, cap);
    if (!buffer) {
        return -1;
    }
    ssize_t total = 0;
    memset(req, 0, sizeof(*req));

    while (total < (ssize_t)(cap - 1)) {
        ssize_t n = recv(fd, buffer + total, cap - 1 - (size_t)total, 0);
        if (n <= 0) {
            free(buffer);
            return -1;
        }
        total += n;
        buffer[total] = '\0';
        if (strstr(buffer, "\r\n\r\n")) {
            break;
        }
    }

    char *headers_end = strstr(buffer, "\r\n\r\n");
    if (!headers_end) {
        free(buffer);
        return -1;
    }
    size_t header_len = (size_t)(headers_end - buffer);
    size_t body_offset = header_len + 4;

    char *line_end = strstr(buffer, "\r\n");
    if (!line_end) {
        free(buffer);
        return -1;
    }
    *line_end = '\0';
    sscanf(buffer, "%7s %255s", req->method, req->path);

    char *q = strchr(req->path, '?');
    if (q) {
        copy_string(req->query, sizeof(req->query), q + 1);
        *q = '\0';
    }

    size_t content_length = 0;
    char *line = line_end + 2;
    while (line < headers_end) {
        char *next = strstr(line, "\r\n");
        if (!next) {
            break;
        }
        *next = '\0';
        if (strncasecmp(line, "Authorization:", 14) == 0) {
            copy_string(req->authorization, sizeof(req->authorization), line + 14);
            trim_whitespace(req->authorization);
        } else if (strncasecmp(line, "Content-Type:", 13) == 0) {
            copy_string(req->content_type, sizeof(req->content_type), line + 13);
            trim_whitespace(req->content_type);
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = (size_t)strtoul(line + 15, NULL, 10);
        }
        line = next + 2;
    }

    size_t already = (size_t)total > body_offset ? (size_t)total - body_offset : 0;
    size_t need = content_length;
    if (need > sizeof(req->body) - 1) {
        need = sizeof(req->body) - 1;
    }
    if (already > need) {
        already = need;
    }
    if (need > 0 && already > 0) {
        memcpy(req->body, buffer + body_offset, already);
    }
    while (already < need) {
        ssize_t n = recv(fd, req->body + already, need - already, 0);
        if (n <= 0) {
            free(buffer);
            return -1;
        }
        already += (size_t)n;
    }
    req->body[already] = '\0';
    req->body_len = already;
    free(buffer);
    return 0;
}

static int send_all(app_socket_t fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static void send_response(app_socket_t fd, const char *status, const char *content_type, const char *body) {
    size_t body_len = body ? strlen(body) : 0;
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        status, content_type, body_len);
    send_all(fd, header, (size_t)n);
    if (body_len > 0) {
        send_all(fd, body, body_len);
    }
}

static void send_redirect(app_socket_t fd, const char *location) {
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 303 See Other\r\n"
        "Location: %s\r\n"
        "Connection: close\r\n\r\n", location);
    send_all(fd, header, (size_t)n);
}

static void send_unauthorized(app_socket_t fd) {
    const char *body = "<html><body><h1>401 Unauthorized</h1></body></html>";
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"PropagationBot\"\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", strlen(body));
    send_all(fd, header, (size_t)n);
    send_all(fd, body, strlen(body));
}

static int get_form_value(const char *body, const char *key, char *out, size_t out_len) {
    if (!body || !key) {
        return -1;
    }
    size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) {
            break;
        }
        const char *amp = strchr(eq + 1, '&');
        size_t this_key_len = (size_t)(eq - p);
        if (this_key_len == key_len && strncmp(p, key, key_len) == 0) {
            size_t val_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            if (val_len >= out_len) {
                val_len = out_len - 1;
            }
            memcpy(out, eq + 1, val_len);
            out[val_len] = '\0';
            url_decode_inplace(out);
            return 0;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    if (out && out_len > 0) {
        out[0] = '\0';
    }
    return -1;
}

static int auth_ok(const settings_t *settings, const http_request_t *req) {
    if (!settings->admin_user[0] && !settings->admin_password[0]) {
        return 1;
    }
    if (strncasecmp(req->authorization, "Basic ", 6) != 0) {
        return 0;
    }
    unsigned char decoded[512];
    size_t decoded_len = 0;
    if (base64_decode(req->authorization + 6, decoded, &decoded_len) != 0) {
        return 0;
    }
    decoded[decoded_len] = '\0';
    char expected[256];
    snprintf(expected, sizeof(expected), "%s:%s", settings->admin_user, settings->admin_password);
    return strcmp((char *)decoded, expected) == 0;
}

static int path_needs_auth(const char *path) {
    return strcmp(path, "/api/onebot") != 0;
}

static void append_input(sb_t *sb, const char *name, const char *label, const char *value, const char *type) {
    sb_append(sb, "<label><span>");
    html_escape_to_sb(sb, label);
    sb_append(sb, "</span><input type=\"");
    sb_append(sb, type ? type : "text");
    sb_append(sb, "\" name=\"");
    html_escape_to_sb(sb, name);
    sb_append(sb, "\" value=\"");
    html_escape_to_sb(sb, value ? value : "");
    sb_append(sb, "\"></label>");
}

static void append_input_int(sb_t *sb, const char *name, const char *label, int value) {
    char temp[32];
    snprintf(temp, sizeof(temp), "%d", value);
    append_input(sb, name, label, temp, "number");
}

static void append_input_double(sb_t *sb, const char *name, const char *label, double value) {
    char temp[64];
    snprintf(temp, sizeof(temp), "%.6f", value);
    append_input(sb, name, label, temp, "text");
}

static void append_textarea(sb_t *sb, const char *name, const char *label, const char *value, int rows) {
    sb_append(sb, "<label><span>");
    html_escape_to_sb(sb, label);
    sb_append(sb, "</span><textarea name=\"");
    html_escape_to_sb(sb, name);
    sb_appendf(sb, "\" rows=\"%d\">", rows);
    html_escape_to_sb(sb, value ? value : "");
    sb_append(sb, "</textarea></label>");
}

static void append_select_yesno(sb_t *sb, const char *name, const char *label, int enabled) {
    sb_append(sb, "<label><span>");
    html_escape_to_sb(sb, label);
    sb_append(sb, "</span><select name=\"");
    html_escape_to_sb(sb, name);
    sb_append(sb, "\">");
    sb_appendf(sb, "<option value=\"1\"%s>启用</option>", enabled ? " selected" : "");
    sb_appendf(sb, "<option value=\"0\"%s>关闭</option>", enabled ? "" : " selected");
    sb_append(sb, "</select></label>");
}

static void append_select_mode(sb_t *sb, const char *name, const char *label, const char *value) {
    const char *current = value && *value ? value : "全部";
    sb_append(sb, "<label><span>");
    html_escape_to_sb(sb, label);
    sb_append(sb, "</span><select name=\"");
    html_escape_to_sb(sb, name);
    sb_append(sb, "\">");
    sb_appendf(sb, "<option value=\"全部\"%s>全部</option>", strcmp(current, "全部") == 0 ? " selected" : "");
    sb_appendf(sb, "<option value=\"线性\"%s>线性</option>", strcmp(current, "线性") == 0 ? " selected" : "");
    sb_appendf(sb, "<option value=\"非线性\"%s>非线性</option>", strcmp(current, "非线性") == 0 ? " selected" : "");
    sb_append(sb, "</select></label>");
}

static void render_targets_table(app_t *app, sb_t *html) {
    target_t targets[MAX_TARGETS];
    int count = 0;
    storage_load_targets(app, targets, MAX_TARGETS, &count);
    for (int i = 0; i < count; ++i) {
        sb_append(html, "<tr><td>");
        html_escape_to_sb(html, targets[i].label);
        sb_append(html, "</td><td>");
        sb_append(html, targets[i].type == TARGET_PRIVATE ? "私聊" : "群聊");
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, targets[i].target_id);
        sb_append(html, "</td><td>");
        sb_append(html, targets[i].enabled ? "启用" : "关闭");
        sb_append(html, "</td><td>");
        sb_append(html, targets[i].command_enabled ? "允许" : "禁止");
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, targets[i].notes);
        sb_append(html, "</td><td class=\"actions\">");
        sb_appendf(html,
            "<form method=\"post\" action=\"/targets/toggle\"><input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<input type=\"hidden\" name=\"enabled\" value=\"%d\"><button type=\"submit\">%s</button></form>",
            targets[i].id, targets[i].enabled ? 0 : 1, targets[i].enabled ? "停用" : "启用");
        sb_appendf(html,
            "<form method=\"post\" action=\"/targets/delete\"><input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<button type=\"submit\" class=\"ghost\">删除</button></form>",
            targets[i].id);
        sb_append(html, "</td></tr>");
    }
    if (count == 0) {
        sb_append(html, "<tr><td colspan=\"7\">还没有推送目标</td></tr>");
    }
}

static void render_schedule_table(app_t *app, sb_t *html) {
    schedule_rule_t rules[MAX_SCHEDULES];
    int count = 0;
    storage_load_schedules(app, rules, MAX_SCHEDULES, &count);
    for (int i = 0; i < count; ++i) {
        sb_append(html, "<tr><td>");
        html_escape_to_sb(html, rules[i].label);
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, rules[i].report_kind);
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, rules[i].hhmm);
        sb_append(html, "</td><td>");
        sb_append(html, rules[i].enabled ? "启用" : "关闭");
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, rules[i].last_fire_date);
        sb_append(html, "</td><td class=\"actions\">");
        sb_appendf(html,
            "<form method=\"post\" action=\"/schedules/toggle\"><input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<input type=\"hidden\" name=\"enabled\" value=\"%d\"><button type=\"submit\">%s</button></form>",
            rules[i].id, rules[i].enabled ? 0 : 1, rules[i].enabled ? "停用" : "启用");
        sb_appendf(html,
            "<form method=\"post\" action=\"/schedules/delete\"><input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<button type=\"submit\" class=\"ghost\">删除</button></form>",
            rules[i].id);
        sb_append(html, "</td></tr>");
    }
    if (count == 0) {
        sb_append(html, "<tr><td colspan=\"6\">还没有定时任务</td></tr>");
    }
}

static void render_satellite_table(app_t *app, sb_t *html) {
    satellite_t sats[MAX_SATELLITES];
    int count = 0;
    storage_load_satellites(app, sats, MAX_SATELLITES, &count);
    for (int i = 0; i < count; ++i) {
        sb_append(html, "<tr><td>");
        html_escape_to_sb(html, sats[i].name);
        sb_append(html, "</td><td>");
        sb_appendf(html, "%d", sats[i].norad_id);
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, sats[i].mode_type);
        sb_append(html, "</td><td>");
        sb_append(html, sats[i].enabled ? "启用" : "关闭");
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, sats[i].notes);
        sb_append(html, "</td><td class=\"actions\">");
        sb_appendf(html,
            "<form method=\"post\" action=\"/satellites/toggle\"><input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<input type=\"hidden\" name=\"enabled\" value=\"%d\"><button type=\"submit\">%s</button></form>",
            sats[i].id, sats[i].enabled ? 0 : 1, sats[i].enabled ? "停用" : "启用");
        sb_appendf(html,
            "<form method=\"post\" action=\"/satellites/delete\"><input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<button type=\"submit\" class=\"ghost\">删除</button></form>",
            sats[i].id);
        sb_append(html, "</td></tr>");
    }
    if (count == 0) {
        sb_append(html, "<tr><td colspan=\"6\">还没有卫星配置</td></tr>");
    }
}

static void render_satellite_pass_rows(const satellite_summary_t *satellite, sb_t *html) {
    if (!satellite || satellite->pass_count == 0) {
        sb_append(html, "<tr><td colspan=\"5\">今日筛选窗口内暂无符合条件的过境</td></tr>");
        return;
    }
    for (int i = 0; i < satellite->pass_count; ++i) {
        sb_append(html, "<tr>");
        sb_append(html, "<td>");
        html_escape_to_sb(html, satellite->passes[i].name);
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, satellite->passes[i].mode_type);
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, satellite->passes[i].start_local);
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, satellite->passes[i].max_local);
        sb_append(html, "</td><td>");
        sb_appendf(html, "%.0f°", satellite->passes[i].max_elevation);
        sb_append(html, "</td></tr>");
    }
}

static void save_form_setting(app_t *app, const char *body, const char *key) {
    char value[16384];
    if (get_form_value(body, key, value, sizeof(value)) == 0) {
        storage_save_setting(app, key, value);
    }
}

static void json_escape_to_sb_local(sb_t *sb, const char *text) {
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

static int snapshot_has_remote_data(const snapshot_t *snapshot) {
    return snapshot->hamqsl.valid || snapshot->weather.valid || snapshot->tropo.valid ||
        snapshot->meteor.valid || snapshot->satellite.valid;
}

static void append_refresh_status_json(app_t *app, sb_t *json) {
    snapshot_t snapshot;
    int running = 0;
    int last_rc = 0;
    time_t started_at = 0;
    time_t finished_at = 0;
    char reason[MAX_TEXT] = {0};
    char status[MAX_TEXT] = {0};
    time_t now = time(NULL);
    char refreshed_text[MAX_TEXT] = "尚未刷新";
    char started_text[MAX_TEXT] = "";
    char finished_text[MAX_TEXT] = "";
    long long age_seconds = -1;
    int needs_refresh = 0;

    pthread_mutex_lock(&app->cache_mutex);
    snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    pthread_mutex_lock(&app->async_mutex);
    running = app->async_refresh_running;
    last_rc = app->async_refresh_last_rc;
    started_at = app->async_refresh_started_at;
    finished_at = app->async_refresh_finished_at;
    copy_string(reason, sizeof(reason), app->async_refresh_reason);
    copy_string(status, sizeof(status), app->async_refresh_status);
    pthread_mutex_unlock(&app->async_mutex);

    if (snapshot.refreshed_at > 0) {
        format_time_local(snapshot.refreshed_at, refreshed_text, sizeof(refreshed_text));
        age_seconds = (long long)difftime(now, snapshot.refreshed_at);
    }
    if (started_at > 0) {
        format_time_local(started_at, started_text, sizeof(started_text));
    }
    if (finished_at > 0) {
        format_time_local(finished_at, finished_text, sizeof(finished_text));
    }
    needs_refresh = !snapshot_has_remote_data(&snapshot) || snapshot.refreshed_at == 0;

    sb_append(json, "{");
    sb_appendf(json, "\"refreshing\":%s,", running ? "true" : "false");
    sb_append(json, "\"status\":\"");
    json_escape_to_sb_local(json, status[0] ? status : "空闲");
    sb_append(json, "\",\"reason\":\"");
    json_escape_to_sb_local(json, reason);
    sb_append(json, "\",");
    sb_appendf(json, "\"last_rc\":%d,", last_rc);
    sb_appendf(json, "\"last_refreshed_at\":%lld,", (long long)snapshot.refreshed_at);
    sb_append(json, "\"last_refreshed_text\":\"");
    json_escape_to_sb_local(json, refreshed_text);
    sb_append(json, "\",");
    sb_appendf(json, "\"snapshot_age_seconds\":%lld,", age_seconds);
    sb_appendf(json, "\"started_at\":%lld,", (long long)started_at);
    sb_append(json, "\"started_text\":\"");
    json_escape_to_sb_local(json, started_text);
    sb_append(json, "\",");
    sb_appendf(json, "\"finished_at\":%lld,", (long long)finished_at);
    sb_append(json, "\"finished_text\":\"");
    json_escape_to_sb_local(json, finished_text);
    sb_append(json, "\",");
    sb_appendf(json, "\"needs_refresh\":%s", needs_refresh ? "true" : "false");
    sb_append(json, "}");
}

static void save_settings_from_form(app_t *app, const char *body) {
    const char *keys[] = {
        "bind_addr", "http_port", "admin_user", "admin_password",
        "station_name", "station_grid", "psk_grids", "latitude", "longitude", "altitude_m", "timezone",
        "onebot_api_base", "onebot_access_token", "onebot_webhook_token", "bot_name", "bot_qq", "bot_password",
        "onebot_send_delay_ms", "onebot_retry_count", "onebot_retry_delay_ms",
        "hamqsl_interval_minutes", "weather_interval_minutes", "tropo_interval_minutes",
        "meteor_interval_hours", "satellite_interval_hours", "psk_eval_interval_seconds", "snapshot_rebuild_seconds",
        "psk_radius_km", "psk_window_minutes", "rate_limit_per_minute",
        "geomag_alert_enabled", "geomag_alert_threshold_g",
        "sixm_alert_enabled", "sixm_alert_interval_minutes", "sixm_psk_trigger_spots",
        "tropo_source_url", "tropo_forecast_hours", "tropo_send_image",
        "meteor_source_url", "meteor_enabled", "meteor_selected_showers", "meteor_max_items",
        "satellite_source_url", "satellite_api_base", "satellite_api_key", "satellite_enabled",
        "satellite_days", "satellite_min_elevation", "satellite_window_start", "satellite_window_end",
        "satellite_mode_filter", "satellite_max_items",
        "hamqsl_widget_url", "hamqsl_selected_fields", "include_source_urls", "include_hamqsl_widget",
        "report_template_full", "report_template_6m", "report_template_solar",
        "report_template_geomag", "report_template_open6m", "help_template",
        "compact_template_hamqsl", "compact_template_hamqsl_unavailable",
        "section_template_weather", "section_template_weather_unavailable",
        "section_template_tropo", "section_template_tropo_unavailable",
        "section_template_solar", "section_template_solar_unavailable",
        "compact_template_meteor", "compact_template_meteor_unavailable",
        "section_template_satellite", "section_template_satellite_unavailable",
        "section_template_6m", "section_template_analysis",
        "compact_template_hamqsl_image",
        "report_template_pskmap", "report_template_pskmap_failed",
        "trigger_full", "trigger_6m", "trigger_solar", "trigger_help", "trigger_pskmap"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        save_form_setting(app, body, keys[i]);
    }
    storage_load_settings(app, &app->settings);
    apply_timezone(app->settings.timezone);
    app_rebuild_snapshot(app);
}

static void handle_add_target(app_t *app, const char *body) {
    target_t target;
    memset(&target, 0, sizeof(target));
    char type_text[32] = {0};
    char enabled_text[32] = {0};
    char command_text[32] = {0};
    get_form_value(body, "label", target.label, sizeof(target.label));
    get_form_value(body, "target_id", target.target_id, sizeof(target.target_id));
    get_form_value(body, "notes", target.notes, sizeof(target.notes));
    get_form_value(body, "type", type_text, sizeof(type_text));
    get_form_value(body, "enabled", enabled_text, sizeof(enabled_text));
    get_form_value(body, "command_enabled", command_text, sizeof(command_text));
    target.type = strcmp(type_text, "private") == 0 ? TARGET_PRIVATE : TARGET_GROUP;
    target.enabled = enabled_text[0] ? atoi(enabled_text) : 1;
    target.command_enabled = command_text[0] ? atoi(command_text) : 1;
    if (target.label[0] && target.target_id[0]) {
        storage_add_target(app, &target);
    }
}

static void handle_add_schedule(app_t *app, const char *body) {
    schedule_rule_t rule;
    memset(&rule, 0, sizeof(rule));
    char enabled_text[16] = {0};
    get_form_value(body, "label", rule.label, sizeof(rule.label));
    get_form_value(body, "report_kind", rule.report_kind, sizeof(rule.report_kind));
    get_form_value(body, "hhmm", rule.hhmm, sizeof(rule.hhmm));
    get_form_value(body, "enabled", enabled_text, sizeof(enabled_text));
    rule.enabled = enabled_text[0] ? atoi(enabled_text) : 1;
    if (rule.label[0] && rule.report_kind[0] && parse_hhmm(rule.hhmm) >= 0) {
        storage_add_schedule(app, &rule);
    }
}

static void handle_add_satellite(app_t *app, const char *body) {
    satellite_t sat;
    memset(&sat, 0, sizeof(sat));
    char enabled_text[16] = {0};
    char norad_text[32] = {0};
    get_form_value(body, "name", sat.name, sizeof(sat.name));
    get_form_value(body, "norad_id", norad_text, sizeof(norad_text));
    get_form_value(body, "mode_type", sat.mode_type, sizeof(sat.mode_type));
    get_form_value(body, "notes", sat.notes, sizeof(sat.notes));
    get_form_value(body, "enabled", enabled_text, sizeof(enabled_text));
    sat.norad_id = atoi(norad_text);
    sat.enabled = enabled_text[0] ? atoi(enabled_text) : 1;
    if (sat.name[0] && sat.norad_id > 0) {
        if (!sat.mode_type[0]) {
            copy_string(sat.mode_type, sizeof(sat.mode_type), "线性");
        }
        storage_add_satellite(app, &sat);
    }
}

static char *render_dashboard(app_t *app) {
    settings_t settings;
    snapshot_t snapshot;
    int async_running = 0;
    int async_last_rc = 0;
    char async_status[MAX_TEXT] = {0};
    char refreshed_text[MAX_TEXT] = "尚未刷新";
    char refreshed_age_text[MAX_TEXT] = "缓存尚未建立";
    int auto_refresh = 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);
    pthread_mutex_lock(&app->async_mutex);
    async_running = app->async_refresh_running;
    async_last_rc = app->async_refresh_last_rc;
    copy_string(async_status, sizeof(async_status), app->async_refresh_status);
    pthread_mutex_unlock(&app->async_mutex);

    if (snapshot.refreshed_at > 0) {
        long long age = (long long)difftime(now, snapshot.refreshed_at);
        format_time_local(snapshot.refreshed_at, refreshed_text, sizeof(refreshed_text));
        snprintf(refreshed_age_text, sizeof(refreshed_age_text), "缓存年龄 %lld 秒", age);
    }
    auto_refresh = !async_running &&
        (!snapshot_has_remote_data(&snapshot) ||
        snapshot.refreshed_at == 0 ||
        difftime(now, snapshot.refreshed_at) >= clamp_int(settings.snapshot_rebuild_seconds, 15, 600));

    sb_t recent_spots, logs_rows, targets_rows, schedule_rows, satellite_rows, page;
    sb_init(&recent_spots);
    sb_init(&logs_rows);
    sb_init(&targets_rows);
    sb_init(&schedule_rows);
    sb_init(&satellite_rows);
    sb_init(&page);

    psk_append_recent_rows(app, &recent_spots, &settings, 12);
    storage_load_recent_logs(app, &logs_rows);
    render_targets_table(app, &targets_rows);
    render_schedule_table(app, &schedule_rows);
    render_satellite_table(app, &satellite_rows);

    sb_append(&page,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>传播后台</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
        "<style>"
        ":root{--bg:#f6f1e7;--card:#fffdfa;--line:#dbcab3;--ink:#2f2419;--muted:#7b6a58;--accent:#0f766e;--accent2:#b45309;}"
        "body{margin:0;background:radial-gradient(circle at top,#fff8ee,transparent 45%),linear-gradient(180deg,#f4ede2,#efe6d7);color:var(--ink);font:14px/1.5 'Segoe UI','Microsoft YaHei',sans-serif;}"
        ".wrap{max-width:1400px;margin:0 auto;padding:22px;}"
        ".hero{background:linear-gradient(135deg,#fff6e7,#f1fbf9);border:1px solid var(--line);border-radius:24px;padding:24px;box-shadow:0 18px 60px rgba(47,36,25,.08);}"
        ".hero h1{margin:0 0 8px 0;font-size:30px;} .hero p{margin:4px 0;color:var(--muted);}"
        ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px;margin-top:18px;}"
        ".card{background:var(--card);border:1px solid var(--line);border-radius:22px;padding:18px;box-shadow:0 10px 30px rgba(47,36,25,.06);}"
        ".card h2{margin:0 0 12px 0;font-size:20px;} .card h3{margin:18px 0 10px 0;font-size:16px;}"
        ".two{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;} .three{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px;}"
        "label{display:block;} label span{display:block;font-size:12px;color:var(--muted);margin-bottom:6px;}"
        "input,select,textarea{width:100%;box-sizing:border-box;border:1px solid #d8cab8;border-radius:14px;padding:10px 12px;background:#fffdf9;color:var(--ink);}"
        "textarea{resize:vertical;} button{border:0;border-radius:14px;padding:10px 14px;background:linear-gradient(135deg,var(--accent),#115e59);color:#fff;cursor:pointer;}"
        "button.ghost{background:#efe4d5;color:var(--ink);} .actions{display:flex;gap:8px;flex-wrap:wrap;}"
        "table{width:100%;border-collapse:collapse;} th,td{padding:9px;border-bottom:1px solid #eadfce;text-align:left;vertical-align:top;}"
        "pre{margin:0;white-space:pre-wrap;background:#1f2937;color:#f8fafc;padding:14px;border-radius:16px;overflow:auto;}"
        ".muted{color:var(--muted);} .pill{display:inline-block;padding:4px 10px;border-radius:999px;background:#efe1cf;color:#7a4b12;margin-right:6px;font-size:12px;}"
        ".toolbar{display:flex;gap:10px;flex-wrap:wrap;}.toolbar form{margin:0;} .help{font-size:12px;color:var(--muted);}"
        "@media (max-width:800px){.two,.three{grid-template-columns:1fr;}}"
        "</style></head><body><div class=\"wrap\">");

    sb_appendf(&page,
        "<section class=\"hero\"><h1>%s</h1><p>台站 %s / %s / 时区 %s</p>"
        "<p>OneBot 回调地址：<code>/api/onebot?token=%s</code></p>"
        "<p><span class=\"pill\">6m %s</span><span class=\"pill\">太阳 %s</span><span class=\"pill\">流星雨 %s</span><span class=\"pill\">卫星 %s / %d 条</span></p>"
        "<p><span class=\"pill\" id=\"refresh-state\">刷新 %s</span><span class=\"pill\" id=\"refresh-time\">%s</span><span class=\"pill\" id=\"refresh-age\">%s</span></p>"
        "<p class=\"help\" id=\"refresh-detail\">%s</p></section>",
        settings.bot_name[0] ? settings.bot_name : APP_NAME,
        settings.station_name,
        settings.station_grid,
        settings.timezone,
        settings.onebot_webhook_token,
        snapshot.psk.assessment,
        snapshot.sun_summary,
        snapshot.meteor.valid ? snapshot.meteor.shower_name : "未获取",
        snapshot.satellite.valid ? snapshot.satellite.api_status : "未获取",
        snapshot.satellite.valid ? snapshot.satellite.pass_count : 0,
        async_running ? "进行中" : (async_status[0] ? async_status : "空闲"),
        refreshed_text,
        refreshed_age_text,
        async_running ? "页面已先用缓存秒开，后台正在抓取最新数据，完成后会自动更新。" :
        (async_last_rc < 0 ? "上次后台刷新失败，当前先显示已有缓存，可稍后再试一次。" :
        "页面现在默认直接显示缓存，打开后会在后台异步更新，不再阻塞界面。"));

    sb_append(&page, "<div class=\"grid\">");
    sb_append(&page, "<section class=\"card\"><h2>当前分析</h2><p>");
    html_escape_to_sb(&page, snapshot.analysis_summary);
    sb_append(&page, "</p><div class=\"toolbar\">"
        "<form method=\"post\" action=\"/actions/refresh\" id=\"refresh-form\"><button type=\"submit\" id=\"refresh-button\">立即刷新</button></form>"
        "<form method=\"post\" action=\"/actions/send\"><input type=\"hidden\" name=\"kind\" value=\"full\"><button type=\"submit\">发送完整简报</button></form>"
        "<form method=\"post\" action=\"/actions/send\"><input type=\"hidden\" name=\"kind\" value=\"6m\"><button type=\"submit\">发送 6m 简报</button></form>"
        "<form method=\"post\" action=\"/actions/send\"><input type=\"hidden\" name=\"kind\" value=\"solar\"><button type=\"submit\">发送太阳简报</button></form>"
        "<form method=\"post\" action=\"/actions/send\"><input type=\"hidden\" name=\"kind\" value=\"pskmap\"><button type=\"submit\">发送 PSK 图</button></form>"
        "</div></section>");

    sb_append(&page, "<section class=\"card\"><h2>消息预览</h2><pre>");
    html_escape_to_sb(&page, snapshot.report_text);
    sb_append(&page, "</pre></section>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"grid\">");
    sb_append(&page, "<section class=\"card\"><h2>基础与机器人</h2><form method=\"post\" action=\"/settings/save\"><div class=\"three\">");
    append_input(&page, "station_name", "台站呼号", settings.station_name, "text");
    append_input(&page, "station_grid", "主网格", settings.station_grid, "text");
    append_input(&page, "psk_grids", "PSK 监控网格", settings.psk_grids, "text");
    append_input_double(&page, "latitude", "纬度", settings.latitude);
    append_input_double(&page, "longitude", "经度", settings.longitude);
    append_input_double(&page, "altitude_m", "海拔(m)", settings.altitude_m);
    sb_append(&page, "</div><div class=\"three\">");
    append_input(&page, "timezone", "时区", settings.timezone, "text");
    append_input(&page, "bind_addr", "监听地址", settings.bind_addr, "text");
    append_input_int(&page, "http_port", "后台端口", settings.http_port);
    append_input(&page, "admin_user", "后台账号", settings.admin_user, "text");
    append_input(&page, "admin_password", "后台密码", settings.admin_password, "password");
    append_input_int(&page, "rate_limit_per_minute", "每分钟问答限额", settings.rate_limit_per_minute);
    sb_append(&page, "</div><h3>QQ / OneBot</h3><div class=\"three\">");
    append_input(&page, "bot_name", "机器人名字", settings.bot_name, "text");
    append_input(&page, "bot_qq", "机器人 QQ", settings.bot_qq, "text");
    append_input(&page, "bot_password", "机器人密码(仅保存)", settings.bot_password, "password");
    append_input(&page, "onebot_api_base", "OneBot API 地址", settings.onebot_api_base, "text");
    append_input(&page, "onebot_access_token", "OneBot Access Token", settings.onebot_access_token, "text");
    append_input(&page, "onebot_webhook_token", "回调令牌", settings.onebot_webhook_token, "text");
    append_input_int(&page, "onebot_send_delay_ms", "群发间隔(ms)", settings.onebot_send_delay_ms);
    append_input_int(&page, "onebot_retry_count", "失败重试次数", settings.onebot_retry_count);
    append_input_int(&page, "onebot_retry_delay_ms", "重试等待(ms)", settings.onebot_retry_delay_ms);
    sb_append(&page, "</div><p class=\"help\">建议真人号场景保持群发间隔 1000ms 以上，并保留至少 1 次失败重试，减少 NapCat 或网络抖动造成的漏发。</p><h3>抓取频率</h3><div class=\"three\">");
    append_input_int(&page, "hamqsl_interval_minutes", "HAMqsl 分钟", settings.hamqsl_interval_minutes);
    append_input_int(&page, "weather_interval_minutes", "天气分钟", settings.weather_interval_minutes);
    append_input_int(&page, "tropo_interval_minutes", "Tropo 分钟", settings.tropo_interval_minutes);
    append_input_int(&page, "meteor_interval_hours", "流星雨小时", settings.meteor_interval_hours);
    append_input_int(&page, "satellite_interval_hours", "卫星小时", settings.satellite_interval_hours);
    append_input_int(&page, "psk_eval_interval_seconds", "PSK 秒", settings.psk_eval_interval_seconds);
    append_input_int(&page, "snapshot_rebuild_seconds", "重建快照秒", settings.snapshot_rebuild_seconds);
    append_input_int(&page, "psk_radius_km", "本地半径 km", settings.psk_radius_km);
    append_input_int(&page, "psk_window_minutes", "PSK 窗口分钟", settings.psk_window_minutes);
    sb_append(&page, "</div><h3>告警与来源</h3><div class=\"three\">");
    append_select_yesno(&page, "geomag_alert_enabled", "地磁告警", settings.geomag_alert_enabled);
    append_input_int(&page, "geomag_alert_threshold_g", "地磁阈值 G", settings.geomag_alert_threshold_g);
    append_select_yesno(&page, "sixm_alert_enabled", "6m 告警", settings.sixm_alert_enabled);
    append_input_int(&page, "sixm_alert_interval_minutes", "6m 重发分钟", settings.sixm_alert_interval_minutes);
    append_input_int(&page, "sixm_psk_trigger_spots", "PSK 触发条数", settings.sixm_psk_trigger_spots);
    append_input(&page, "tropo_source_url", "F5LEN 页面", settings.tropo_source_url, "text");
    append_input_int(&page, "tropo_forecast_hours", "Tropo 预测小时", settings.tropo_forecast_hours);
    append_select_yesno(&page, "tropo_send_image", "发送 Tropo 图", settings.tropo_send_image);
    append_input(&page, "meteor_source_url", "流星雨来源", settings.meteor_source_url, "text");
    append_select_yesno(&page, "meteor_enabled", "流星雨提醒", settings.meteor_enabled);
    append_input(&page, "meteor_selected_showers", "流星雨筛选(csv)", settings.meteor_selected_showers, "text");
    append_input_int(&page, "meteor_max_items", "流星雨最多条数", settings.meteor_max_items);
    sb_append(&page, "<p class=\"help\">流星雨筛选现在支持中英文名称混填，例如：Lyrids, 天琴座流星雨。</p>");
    append_input(&page, "hamqsl_widget_url", "HAMqsl 小组件图", settings.hamqsl_widget_url, "text");
    append_select_yesno(&page, "include_hamqsl_widget", "发送 HAMqsl 图", settings.include_hamqsl_widget);
    append_select_yesno(&page, "include_source_urls", "附带来源网址", settings.include_source_urls);
    sb_append(&page, "</div><h3>卫星过滤</h3><div class=\"three\">");
    append_input(&page, "satellite_source_url", "卫星来源说明", settings.satellite_source_url, "text");
    append_input(&page, "satellite_api_base", "卫星 API 地址", settings.satellite_api_base, "text");
    append_input(&page, "satellite_api_key", "卫星 API Key", settings.satellite_api_key, "text");
    append_select_yesno(&page, "satellite_enabled", "卫星推荐", settings.satellite_enabled);
    append_input_int(&page, "satellite_days", "查询天数", settings.satellite_days);
    append_input_int(&page, "satellite_min_elevation", "最低仰角", settings.satellite_min_elevation);
    append_input(&page, "satellite_window_start", "开始时间", settings.satellite_window_start, "text");
    append_input(&page, "satellite_window_end", "结束时间", settings.satellite_window_end, "text");
    append_select_mode(&page, "satellite_mode_filter", "模式过滤", settings.satellite_mode_filter);
    append_input_int(&page, "satellite_max_items", "最多显示条数", settings.satellite_max_items);
    sb_append(&page, "</div>");
    append_textarea(&page, "hamqsl_selected_fields", "HAMqsl 显示字段(csv)", settings.hamqsl_selected_fields, 3);
    sb_append(&page, "<div class=\"toolbar\" style=\"margin-top:12px;\"><button type=\"submit\">保存设置</button></div></form></section>");

    sb_append(&page, "<section class=\"card\"><h2>模板与问词</h2><form method=\"post\" action=\"/settings/save\">");
    append_textarea(&page, "report_template_full", "完整简报模板", settings.report_template_full, 8);
    append_textarea(&page, "report_template_6m", "6m 简报模板", settings.report_template_6m, 6);
    append_textarea(&page, "report_template_solar", "太阳简报模板", settings.report_template_solar, 5);
    append_textarea(&page, "report_template_geomag", "地磁告警模板", settings.report_template_geomag, 4);
    append_textarea(&page, "report_template_open6m", "6m 告警模板", settings.report_template_open6m, 5);
    append_textarea(&page, "help_template", "帮助回复模板", settings.help_template, 4);
    append_textarea(&page, "compact_template_hamqsl", "HAMqsl 精简段模板", settings.compact_template_hamqsl, 5);
    append_textarea(&page, "compact_template_meteor", "流星雨精简段模板", settings.compact_template_meteor, 5);
    append_textarea(&page, "compact_template_hamqsl_image", "HAMqsl 日图段模板", settings.compact_template_hamqsl_image, 3);
    append_textarea(&page, "report_template_pskmap", "PSK 快照模板", settings.report_template_pskmap, 5);
    append_textarea(&page, "report_template_pskmap_failed", "PSK 快照失败模板", settings.report_template_pskmap_failed, 5);
    append_textarea(&page, "compact_template_hamqsl_unavailable", "HAMqsl 不可用模板", settings.compact_template_hamqsl_unavailable, 3);
    append_textarea(&page, "section_template_weather", "天气分段模板", settings.section_template_weather, 6);
    append_textarea(&page, "section_template_weather_unavailable", "天气不可用模板", settings.section_template_weather_unavailable, 3);
    append_textarea(&page, "section_template_tropo", "Tropo 分段模板", settings.section_template_tropo, 5);
    append_textarea(&page, "section_template_tropo_unavailable", "Tropo 不可用模板", settings.section_template_tropo_unavailable, 3);
    append_textarea(&page, "section_template_solar", "太阳分段模板", settings.section_template_solar, 5);
    append_textarea(&page, "section_template_solar_unavailable", "太阳不可用模板", settings.section_template_solar_unavailable, 3);
    append_textarea(&page, "compact_template_meteor_unavailable", "流星雨不可用模板", settings.compact_template_meteor_unavailable, 3);
    append_textarea(&page, "section_template_satellite", "卫星分段模板", settings.section_template_satellite, 5);
    append_textarea(&page, "section_template_satellite_unavailable", "卫星不可用模板", settings.section_template_satellite_unavailable, 3);
    append_textarea(&page, "section_template_6m", "6m 分段模板", settings.section_template_6m, 6);
    append_textarea(&page, "section_template_analysis", "综合分析模板", settings.section_template_analysis, 4);
    append_textarea(&page, "compact_template_hamqsl_image", "来源 / 图片分段模板", settings.compact_template_hamqsl_image, 4);
    sb_append(&page, "<div class=\"three\">");
    append_input(&page, "trigger_full", "完整简报问词", settings.trigger_full, "text");
    append_input(&page, "trigger_6m", "6m 问词", settings.trigger_6m, "text");
    append_input(&page, "trigger_solar", "太阳问词", settings.trigger_solar, "text");
    sb_append(&page, "</div>");
    sb_append(&page, "<div class=\"two\">");
    append_input(&page, "trigger_help", "帮助问词", settings.trigger_help, "text");
    append_input(&page, "trigger_pskmap", "PSK 图问词", settings.trigger_pskmap, "text");
    sb_append(&page, "</div>");
    sb_append(&page, "<p class=\"help\">可用模板标记：{{bot_name}} {{station_name}} {{station_grid}} {{psk_grids}} {{section_hamqsl}} {{section_weather}} {{section_tropo}} {{section_6m}} {{section_solar}} {{section_meteor}} {{section_satellite}} {{section_sources}} {{analysis_summary}} {{refreshed_at}} {{geomag_g}} {{sixm_alert_level}} {{updated}} {{kindex}} {{geomagfield}} {{hf_day}} {{hf_night}} {{ham_solarflux}} {{ham_aindex}} {{ham_kindex}} {{ham_xray}} {{ham_sunspots}} {{ham_muf}} {{weather_level}} {{weather_score}} {{tropo_category}} {{tropo_score}} {{meteor_name}} {{meteor_name_cn}} {{meteor_peak}} {{meteor_days_left}} {{peak_date}} {{peak_date_cn}} {{days_left}} {{countdown_text}} {{moon_percent}} {{hamqsl_widget_url}}</p><p class=\"help\">推荐做法：完整简报里保留 {{section_hamqsl}} / {{section_meteor}} / {{section_sources}}，需要更细时再在下面三个精简模板里改固定文字。</p><div class=\"toolbar\"><button type=\"submit\">保存模板</button></div></form></section>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"grid\">");
    sb_append(&page, "<section class=\"card\"><h2>推送目标</h2><table><tr><th>名称</th><th>类型</th><th>ID</th><th>推送</th><th>问答</th><th>备注</th><th>操作</th></tr>");
    sb_append(&page, targets_rows.data ? targets_rows.data : "");
    sb_append(&page, "</table><h3>新增目标</h3><form method=\"post\" action=\"/targets/add\"><div class=\"three\">");
    append_input(&page, "label", "名称", "", "text");
    sb_append(&page, "<label><span>类型</span><select name=\"type\"><option value=\"group\">群聊</option><option value=\"private\">私聊</option></select></label>");
    append_input(&page, "target_id", "群号 / QQ", "", "text");
    append_select_yesno(&page, "enabled", "启用推送", 1);
    append_select_yesno(&page, "command_enabled", "允许问答", 1);
    append_input(&page, "notes", "备注", "", "text");
    sb_append(&page, "</div><div class=\"toolbar\" style=\"margin-top:12px;\"><button type=\"submit\">添加目标</button></div></form></section>");

    sb_append(&page, "<section class=\"card\"><h2>定时任务</h2><table><tr><th>名称</th><th>类型</th><th>时间</th><th>状态</th><th>上次发送</th><th>操作</th></tr>");
    sb_append(&page, schedule_rows.data ? schedule_rows.data : "");
    sb_append(&page, "</table><h3>新增定时</h3><form method=\"post\" action=\"/schedules/add\"><div class=\"three\">");
    append_input(&page, "label", "任务名", "", "text");
    sb_append(&page, "<label><span>简报类型</span><select name=\"report_kind\"><option value=\"full\">full</option><option value=\"6m\">6m</option><option value=\"solar\">solar</option><option value=\"pskmap\">pskmap</option><option value=\"help\">help</option></select></label>");
    append_input(&page, "hhmm", "时间(HH:MM)", "", "text");
    append_select_yesno(&page, "enabled", "启用", 1);
    sb_append(&page, "</div><div class=\"toolbar\" style=\"margin-top:12px;\"><button type=\"submit\">添加定时</button></div></form></section>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"grid\">");
    sb_append(&page, "<section class=\"card\"><h2>卫星列表</h2><table><tr><th>名称</th><th>NORAD</th><th>模式</th><th>状态</th><th>备注</th><th>操作</th></tr>");
    sb_append(&page, satellite_rows.data ? satellite_rows.data : "");
    sb_append(&page, "</table><h3>新增卫星</h3><form method=\"post\" action=\"/satellites/add\"><div class=\"three\">");
    append_input(&page, "name", "名称", "", "text");
    append_input(&page, "norad_id", "NORAD", "", "number");
    sb_append(&page, "<label><span>模式</span><select name=\"mode_type\"><option value=\"线性\">线性</option><option value=\"非线性\">非线性</option></select></label>");
    append_select_yesno(&page, "enabled", "启用", 1);
    append_input(&page, "notes", "备注", "", "text");
    sb_append(&page, "</div><div class=\"toolbar\" style=\"margin-top:12px;\"><button type=\"submit\">添加卫星</button></div></form></section>");

    sb_append(&page, "<section class=\"card\"><h2>最近 6m Spot</h2><table><tr><th>时间</th><th>命中网格</th><th>发送方</th><th>接收方</th><th>模式</th><th>SNR</th><th>距离</th></tr>");
    sb_append(&page, recent_spots.data ? recent_spots.data : "");
    sb_append(&page, "</table></section>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"grid\">");
    sb_append(&page, "<section class=\"card\"><h2>卫星状态</h2><p>");
    sb_appendf(&page,
        "<span class=\"pill\">API %s</span><span class=\"pill\">已选 %d 颗</span><span class=\"pill\">命中 %d 条</span>",
        snapshot.satellite.valid ? snapshot.satellite.api_status : "未获取",
        snapshot.satellite.valid ? snapshot.satellite.selected_satellites : 0,
        snapshot.satellite.valid ? snapshot.satellite.pass_count : 0);
    sb_append(&page, "</p><table>");
    sb_appendf(&page, "<tr><th>总配置</th><td>%d</td><th>已启用</th><td>%d</td></tr>",
        snapshot.satellite.total_satellites, snapshot.satellite.enabled_satellites);
    sb_appendf(&page, "<tr><th>参与计算</th><td>%d</td><th>API 成功</th><td>%d / %d</td></tr>",
        snapshot.satellite.selected_satellites, snapshot.satellite.api_successes, snapshot.satellite.api_requests);
    sb_appendf(&page, "<tr><th>时间窗</th><td>%s - %s</td><th>最低仰角</th><td>%d°</td></tr>",
        settings.satellite_window_start, settings.satellite_window_end, settings.satellite_min_elevation);
    sb_append(&page, "</table><p class=\"help\">已选卫星：");
    html_escape_to_sb(&page, snapshot.satellite.selected_names[0] ? snapshot.satellite.selected_names : "未选择");
    sb_append(&page, "</p><pre>");
    html_escape_to_sb(&page, snapshot.satellite.summary[0] ? snapshot.satellite.summary : "卫星状态暂不可用。");
    sb_append(&page, "</pre></section>");

    sb_append(&page, "<section class=\"card\"><h2>今日卫星过境</h2><table><tr><th>名称</th><th>模式</th><th>开始</th><th>最大时刻</th><th>最大仰角</th></tr>");
    render_satellite_pass_rows(&snapshot.satellite, &page);
    sb_append(&page, "</table></section>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"grid\">");
    sb_append(&page, "<section class=\"card\"><h2>6m 预览</h2><pre>");
    html_escape_to_sb(&page, snapshot.report_6m);
    sb_append(&page, "</pre></section>");
    sb_append(&page, "<section class=\"card\"><h2>太阳预览</h2><pre>");
    html_escape_to_sb(&page, snapshot.report_solar);
    sb_append(&page, "</pre></section>");
    sb_append(&page, "</div>");

    sb_append(&page, "<section class=\"card\" style=\"margin-top:18px;\"><h2>运行日志</h2><table><tr><th>时间</th><th>级别</th><th>内容</th></tr>");
    sb_append(&page, logs_rows.data ? logs_rows.data : "");
    sb_append(&page, "</table></section>");

    sb_appendf(&page,
        "<script>"
        "const dashboardState={autoRefresh:%s,lastRenderedAt:%lld};"
        "let awaitingReload=false;"
        "async function fetchRefreshStatus(){const res=await fetch('/api/status',{cache:'no-store'});if(!res.ok)return null;return await res.json();}"
        "function setText(id,text){const el=document.getElementById(id);if(el)el.textContent=text;}"
        "function renderRefreshStatus(data){if(!data)return;setText('refresh-state','刷新 '+(data.refreshing?'进行中':(data.status||'空闲')));setText('refresh-time','最后刷新 '+(data.last_refreshed_text||'尚未刷新'));setText('refresh-age',data.snapshot_age_seconds>=0?('缓存年龄 '+data.snapshot_age_seconds+' 秒'):'缓存尚未建立');if(data.refreshing){setText('refresh-detail','后台正在抓取最新传播数据：'+(data.reason||'manual')+'，页面会在完成后自动更新。');}else if(data.last_rc<0){setText('refresh-detail','上次后台刷新失败，当前先显示旧缓存；你可以稍后再次刷新。');}else{setText('refresh-detail','页面已使用缓存秒开，后台刷新完成后会自动显示最新内容。');}const btn=document.getElementById('refresh-button');if(btn){btn.disabled=!!data.refreshing;btn.textContent=data.refreshing?'刷新中...':'立即刷新';}}"
        "async function queueRefresh(reason){const res=await fetch('/api/refresh?reason='+encodeURIComponent(reason||'manual'),{method:'POST',cache:'no-store'});if(!res.ok)return;const data=await res.json();awaitingReload=true;renderRefreshStatus(data);}"
        "async function pollRefreshStatus(){try{const data=await fetchRefreshStatus();renderRefreshStatus(data);if(data&&awaitingReload&&!data.refreshing&&data.last_refreshed_at>dashboardState.lastRenderedAt){window.location.reload();}}catch(e){}}"
        "document.addEventListener('DOMContentLoaded',function(){const form=document.getElementById('refresh-form');if(form){form.addEventListener('submit',function(ev){ev.preventDefault();queueRefresh('manual');});}pollRefreshStatus();setInterval(pollRefreshStatus,3000);if(dashboardState.autoRefresh){queueRefresh('page-load');}});"
        "</script></div></body></html>",
        auto_refresh ? "true" : "false",
        (long long)snapshot.refreshed_at);

    sb_free(&recent_spots);
    sb_free(&logs_rows);
    sb_free(&targets_rows);
    sb_free(&schedule_rows);
    sb_free(&satellite_rows);
    return page.data;
}

static int target_can_reply(app_t *app, target_type_t type, const char *target_id) {
    target_t targets[MAX_TARGETS];
    int count = 0;
    storage_load_targets(app, targets, MAX_TARGETS, &count);
    for (int i = 0; i < count; ++i) {
        if (!targets[i].enabled || !targets[i].command_enabled) {
            continue;
        }
        if (targets[i].type == type && strcmp(targets[i].target_id, target_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int message_matches_trigger_csv(const char *message, const char *csv) {
    if (!message || !csv || !*csv) {
        return 0;
    }
    char temp[MAX_HUGE_TEXT];
    copy_string(temp, sizeof(temp), csv);
    char *save = NULL;
    for (char *part = strtok_r(temp, ",|/ \t\r\n", &save); part; part = strtok_r(NULL, ",|/ \t\r\n", &save)) {
        trim_whitespace(part);
        if (*part && string_contains_ci(message, part)) {
            return 1;
        }
    }
    return 0;
}

static int extract_json_field(const char *json, const char *key, char *out, size_t out_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return -1;
    }
    p = strchr(p, ':');
    if (!p) {
        return -1;
    }
    while (*p && *p != '"' && !isdigit((unsigned char)*p)) {
        p++;
    }
    if (*p == '"') {
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
    size_t i = 0;
    while (*p && (isdigit((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.') && i + 1 < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0 ? 0 : -1;
}

static void handle_onebot_webhook(app_t *app, app_socket_t fd, const http_request_t *req) {
    settings_t settings;
    storage_load_settings(app, &settings);

    char token[256];
    if (settings.onebot_webhook_token[0]) {
        if (get_form_value(req->query, "token", token, sizeof(token)) != 0 ||
            strcmp(token, settings.onebot_webhook_token) != 0) {
            send_response(fd, "403 Forbidden", "text/plain", "forbidden");
            return;
        }
    }

    char post_type[64] = {0};
    char message_type[64] = {0};
    char raw_message[2048] = {0};
    char group_id[64] = {0};
    char user_id[64] = {0};
    extract_json_field(req->body, "post_type", post_type, sizeof(post_type));
    extract_json_field(req->body, "message_type", message_type, sizeof(message_type));
    extract_json_field(req->body, "raw_message", raw_message, sizeof(raw_message));
    extract_json_field(req->body, "group_id", group_id, sizeof(group_id));
    extract_json_field(req->body, "user_id", user_id, sizeof(user_id));

    if (strcmp(post_type, "message") != 0) {
        send_response(fd, "200 OK", "text/plain", "ignored");
        return;
    }
    if (settings.bot_qq[0] && strcmp(user_id, settings.bot_qq) == 0) {
        send_response(fd, "200 OK", "text/plain", "self");
        return;
    }

    target_type_t type = strcmp(message_type, "private") == 0 ? TARGET_PRIVATE : TARGET_GROUP;
    const char *target_id = type == TARGET_PRIVATE ? user_id : group_id;
    if (!target_id[0] || !target_can_reply(app, type, target_id)) {
        send_response(fd, "200 OK", "text/plain", "ignored");
        return;
    }

    char rate_key[MAX_TEXT];
    snprintf(rate_key, sizeof(rate_key), "%s:%s", type == TARGET_PRIVATE ? "private" : "group", target_id);
    if (!app_rate_limit_allow(app, rate_key)) {
        send_response(fd, "200 OK", "text/plain", "rate-limited");
        return;
    }

    refresh_snapshot(app, 0);
    pthread_mutex_lock(&app->cache_mutex);
    snapshot_t snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    const char *reply = NULL;
    if (message_matches_trigger_csv(raw_message, settings.trigger_help)) {
        reply = snapshot.report_help;
    } else if (message_matches_trigger_csv(raw_message, settings.trigger_pskmap)) {
        psk_send_snapshot_image(app, type, target_id);
    } else if (message_matches_trigger_csv(raw_message, settings.trigger_6m)) {
        reply = snapshot.report_6m;
    } else if (message_matches_trigger_csv(raw_message, settings.trigger_solar)) {
        reply = snapshot.report_solar;
    } else if (message_matches_trigger_csv(raw_message, settings.trigger_full)) {
        reply = snapshot.report_text;
    }

    if (reply && *reply) {
        onebot_send_message(app, type, target_id, reply);
    }
    send_response(fd, "200 OK", "text/plain", "ok");
}

static void handle_request(app_t *app, app_socket_t fd, const http_request_t *req) {
    settings_t settings;
    storage_load_settings(app, &settings);
    if (path_needs_auth(req->path) && !auth_ok(&settings, req)) {
        send_unauthorized(fd);
        return;
    }

    if (strcmp(req->path, "/") == 0 && strcmp(req->method, "GET") == 0) {
        char *html = render_dashboard(app);
        send_response(fd, "200 OK", "text/html", html ? html : "");
        free(html);
        return;
    }
    if (strcmp(req->path, "/api/status") == 0 && strcmp(req->method, "GET") == 0) {
        sb_t json;
        sb_init(&json);
        append_refresh_status_json(app, &json);
        send_response(fd, "200 OK", "application/json", json.data ? json.data : "{}");
        sb_free(&json);
        return;
    }
    if (strcmp(req->path, "/api/refresh") == 0 && strcmp(req->method, "POST") == 0) {
        char reason[MAX_TEXT] = {0};
        if (get_form_value(req->query, "reason", reason, sizeof(reason)) != 0) {
            copy_string(reason, sizeof(reason), "manual");
        }
        int rc = app_request_refresh_async(app, 1, 0, reason);
        sb_t json;
        sb_init(&json);
        append_refresh_status_json(app, &json);
        if (rc == -1) {
            send_response(fd, "500 Internal Server Error", "application/json", json.data ? json.data : "{}");
        } else {
            send_response(fd, rc == 0 ? "202 Accepted" : "200 OK", "application/json", json.data ? json.data : "{}");
        }
        sb_free(&json);
        return;
    }
    if (strcmp(req->path, "/settings/save") == 0 && strcmp(req->method, "POST") == 0) {
        save_settings_from_form(app, req->body);
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/targets/add") == 0 && strcmp(req->method, "POST") == 0) {
        handle_add_target(app, req->body);
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/targets/delete") == 0 && strcmp(req->method, "POST") == 0) {
        char id_text[32];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0) {
            storage_delete_target(app, atoi(id_text));
        }
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/targets/toggle") == 0 && strcmp(req->method, "POST") == 0) {
        char id_text[32], enabled_text[16];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0 &&
            get_form_value(req->body, "enabled", enabled_text, sizeof(enabled_text)) == 0) {
            storage_toggle_target(app, atoi(id_text), atoi(enabled_text));
        }
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/schedules/add") == 0 && strcmp(req->method, "POST") == 0) {
        handle_add_schedule(app, req->body);
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/schedules/delete") == 0 && strcmp(req->method, "POST") == 0) {
        char id_text[32];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0) {
            storage_delete_schedule(app, atoi(id_text));
        }
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/schedules/toggle") == 0 && strcmp(req->method, "POST") == 0) {
        char id_text[32], enabled_text[16];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0 &&
            get_form_value(req->body, "enabled", enabled_text, sizeof(enabled_text)) == 0) {
            storage_toggle_schedule(app, atoi(id_text), atoi(enabled_text));
        }
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/satellites/add") == 0 && strcmp(req->method, "POST") == 0) {
        handle_add_satellite(app, req->body);
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/satellites/delete") == 0 && strcmp(req->method, "POST") == 0) {
        char id_text[32];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0) {
            storage_delete_satellite(app, atoi(id_text));
        }
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/satellites/toggle") == 0 && strcmp(req->method, "POST") == 0) {
        char id_text[32], enabled_text[16];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0 &&
            get_form_value(req->body, "enabled", enabled_text, sizeof(enabled_text)) == 0) {
            storage_toggle_satellite(app, atoi(id_text), atoi(enabled_text));
        }
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/actions/refresh") == 0 && strcmp(req->method, "POST") == 0) {
        app_request_refresh_async(app, 1, 0, "manual");
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/actions/send") == 0 && strcmp(req->method, "POST") == 0) {
        char kind[32];
        if (get_form_value(req->body, "kind", kind, sizeof(kind)) != 0) {
            copy_string(kind, sizeof(kind), "full");
        }
        send_report_kind_to_all_targets(app, kind);
        send_redirect(fd, "/");
        return;
    }
    if (strcmp(req->path, "/api/onebot") == 0 && strcmp(req->method, "POST") == 0) {
        handle_onebot_webhook(app, fd, req);
        return;
    }

    send_response(fd, "404 Not Found", "text/plain", "not found");
}

static void *client_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    http_request_t req;
    if (read_http_request(ctx->client_fd, &req) == 0) {
        handle_request(ctx->app, ctx->client_fd, &req);
    }
    close(ctx->client_fd);
    free(ctx);
    return NULL;
}

int http_server_run(app_t *app) {
    app_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    int exit_reason = 0;
    if (fd == APP_INVALID_SOCKET) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)app->settings.http_port);
    if (inet_pton(AF_INET, app->settings.bind_addr, &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        app_set_last_error(app, "HTTP 后台绑定失败: %s:%d，请检查端口是否被占用",
            app->settings.bind_addr, app->settings.http_port);
        app_log(app, "ERROR", "HTTP 绑定失败: %s:%d", app->settings.bind_addr, app->settings.http_port);
        close(fd);
        return -1;
    }
    if (listen(fd, 32) != 0) {
        app_set_last_error(app, "HTTP 后台监听失败: %s:%d",
            app->settings.bind_addr, app->settings.http_port);
        app_log(app, "ERROR", "HTTP 监听失败");
        close(fd);
        return -1;
    }
    app->http_fd = fd;
    app_log(app, "INFO", "HTTP 后台已监听: %s:%d", app->settings.bind_addr, app->settings.http_port);
    if (app->open_admin_console_on_start && !app->admin_console_opened) {
        app_open_admin_console(&app->settings);
        app->admin_console_opened = 1;
    }

    while (app->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        app_socket_t client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == APP_INVALID_SOCKET) {
#ifdef _WIN32
            int accept_error = WSAGetLastError();
            if (accept_error == WSAEINTR) {
                exit_reason = 1;
                continue;
            }
            app_set_last_error(app, "HTTP accept 失败，错误码=%d", accept_error);
            app_log(app, "ERROR", "HTTP accept 失败，错误码=%d", accept_error);
            exit_reason = accept_error;
#else
            if (errno == EINTR) {
                exit_reason = 1;
                continue;
            }
            exit_reason = errno;
#endif
            break;
        }
        client_ctx_t *ctx = calloc(1, sizeof(*ctx));
        if (!ctx) {
            close(client_fd);
            continue;
        }
        ctx->app = app;
        ctx->client_fd = client_fd;
        pthread_t thread;
        if (pthread_create(&thread, NULL, client_thread, ctx) == 0) {
            pthread_detach(thread);
        } else {
            close(client_fd);
            free(ctx);
        }
    }
    app_log(app, "WARN", "HTTP 后台循环已退出: running=%d reason=%d", app->running, exit_reason);
    close(fd);
    return 0;
}
