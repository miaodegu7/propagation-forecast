#include "app.h"

typedef struct {
    app_t *app;
    int client_fd;
} client_ctx_t;

static int read_http_request(int fd, http_request_t *req) {
    char buffer[32768];
    ssize_t total = 0;
    memset(req, 0, sizeof(*req));

    while (total < (ssize_t)(sizeof(buffer) - 1)) {
        ssize_t n = recv(fd, buffer + total, sizeof(buffer) - 1 - (size_t)total, 0);
        if (n <= 0) {
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
        return -1;
    }
    size_t header_len = (size_t)(headers_end - buffer);
    size_t body_offset = header_len + 4;

    char *line_end = strstr(buffer, "\r\n");
    if (!line_end) {
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
            return -1;
        }
        already += (size_t)n;
    }
    req->body[already] = '\0';
    req->body_len = already;
    return 0;
}

static int send_all(int fd, const char *buf, size_t len) {
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

static void send_response(int fd, const char *status, const char *content_type, const char *body) {
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

static void send_redirect(int fd, const char *location) {
    char header[512];
    int n = snprintf(header, sizeof(header),
        "HTTP/1.1 303 See Other\r\n"
        "Location: %s\r\n"
        "Connection: close\r\n\r\n", location);
    send_all(fd, header, (size_t)n);
}

static void send_unauthorized(int fd) {
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

static void render_targets_table(app_t *app, sb_t *html) {
    target_t targets[MAX_TARGETS];
    int count = 0;
    storage_load_targets(app, targets, MAX_TARGETS, &count);
    for (int i = 0; i < count; ++i) {
        sb_append(html, "<tr><td>");
        html_escape_to_sb(html, targets[i].label);
        sb_append(html, "</td><td>");
        sb_append(html, targets[i].type == TARGET_PRIVATE ? "private" : "group");
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, targets[i].target_id);
        sb_append(html, "</td><td>");
        sb_append(html, targets[i].enabled ? "enabled" : "disabled");
        sb_append(html, "</td><td>");
        sb_append(html, targets[i].command_enabled ? "yes" : "no");
        sb_append(html, "</td><td>");
        html_escape_to_sb(html, targets[i].notes);
        sb_append(html, "</td><td>");
        sb_appendf(html,
            "<form method=\"post\" action=\"/targets/toggle\" style=\"display:inline\">"
            "<input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<input type=\"hidden\" name=\"enabled\" value=\"%d\">"
            "<button type=\"submit\">%s</button></form> ",
            targets[i].id, targets[i].enabled ? 0 : 1, targets[i].enabled ? "Disable" : "Enable");
        sb_appendf(html,
            "<form method=\"post\" action=\"/targets/delete\" style=\"display:inline\">"
            "<input type=\"hidden\" name=\"id\" value=\"%d\">"
            "<button type=\"submit\">Delete</button></form>",
            targets[i].id);
        sb_append(html, "</td></tr>");
    }
    if (count == 0) {
        sb_append(html, "<tr><td colspan=\"7\">还没有推送目标</td></tr>");
    }
}

static char *render_dashboard(app_t *app) {
    refresh_snapshot(app, 0);

    settings_t settings;
    snapshot_t snapshot;
    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    sb_t recent_spots;
    sb_t logs_rows;
    sb_t targets_rows;
    sb_t page;
    sb_init(&recent_spots);
    sb_init(&logs_rows);
    sb_init(&targets_rows);
    sb_init(&page);

    psk_append_recent_rows(app, &recent_spots, &settings, 12);
    storage_load_recent_logs(app, &logs_rows);
    render_targets_table(app, &targets_rows);

    sb_append(&page,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<title>Propagation Forecast Bot</title>"
        "<style>"
        "body{font-family:Segoe UI,Arial,sans-serif;background:#f4f6f8;color:#1b1f23;margin:0;padding:24px;}"
        "h1,h2{margin:0 0 12px 0;} .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:18px;}"
        ".card{background:#fff;border-radius:14px;padding:18px;box-shadow:0 8px 24px rgba(0,0,0,.08);}"
        "table{width:100%;border-collapse:collapse;} td,th{padding:8px;border-bottom:1px solid #e6e8eb;text-align:left;vertical-align:top;}"
        "input,select,textarea{width:100%;padding:10px;border:1px solid #ccd2d8;border-radius:10px;box-sizing:border-box;}"
        "button{padding:10px 14px;border:0;border-radius:10px;background:#0a6ebd;color:#fff;cursor:pointer;}"
        ".inline{display:grid;grid-template-columns:1fr 1fr;gap:12px;} .inline3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:12px;}"
        "pre{white-space:pre-wrap;background:#111827;color:#f9fafb;padding:14px;border-radius:12px;overflow:auto;}"
        "small{color:#5b6570;} .muted{color:#5b6570;}"
        "</style></head><body>");

    sb_appendf(&page, "<h1>%s</h1><p class=\"muted\">版本 %s，内置后台，OneBot 回调地址：<code>/api/onebot?token=%s</code></p>",
        APP_NAME, APP_VERSION, settings.onebot_webhook_token);

    sb_append(&page, "<div class=\"grid\">");
    sb_append(&page, "<div class=\"card\"><h2>当前状态</h2><p>");
    html_escape_to_sb(&page, snapshot.sun_summary);
    sb_append(&page, "</p><p>");
    html_escape_to_sb(&page, snapshot.analysis_summary);
    sb_append(&page, "</p><p><strong>6m:</strong> ");
    html_escape_to_sb(&page, snapshot.psk.assessment);
    sb_appendf(&page, " / 分数 %d / 置信度 ", snapshot.psk.score);
    html_escape_to_sb(&page, snapshot.psk.confidence);
    sb_append(&page, "</p></div>");

    sb_append(&page, "<div class=\"card\"><h2>快捷操作</h2>"
        "<form method=\"post\" action=\"/actions/refresh\"><button type=\"submit\">立即刷新</button></form><br>"
        "<form method=\"post\" action=\"/actions/send\"><input type=\"hidden\" name=\"kind\" value=\"full\"><button type=\"submit\">群发完整简报</button></form><br>"
        "<form method=\"post\" action=\"/actions/send\"><input type=\"hidden\" name=\"kind\" value=\"6m\"><button type=\"submit\">群发 6m 简报</button></form><br>"
        "<form method=\"post\" action=\"/actions/send\"><input type=\"hidden\" name=\"kind\" value=\"solar\"><button type=\"submit\">群发太阳简报</button></form>"
        "</div>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"grid\" style=\"margin-top:18px;\">");
    sb_append(&page, "<div class=\"card\"><h2>完整简报预览</h2><pre>");
    html_escape_to_sb(&page, snapshot.report_text);
    sb_append(&page, "</pre></div>");

    sb_append(&page, "<div class=\"card\"><h2>基础配置</h2>"
        "<form method=\"post\" action=\"/settings/save\">"
        "<div class=\"inline3\">"
        "<div><label>台站呼号<input name=\"station_name\" value=\"");
    html_escape_to_sb(&page, settings.station_name);
    sb_append(&page, "\"></label></div><div><label>台站网格<input name=\"station_grid\" value=\"");
    html_escape_to_sb(&page, settings.station_grid);
    sb_append(&page, "\"></label></div><div><label>时区<input name=\"timezone\" value=\"");
    html_escape_to_sb(&page, settings.timezone);
    sb_append(&page, "\"></label></div></div>"
        "<div class=\"inline\">");
    sb_appendf(&page, "<div><label>纬度<input name=\"latitude\" value=\"%.6f\"></label></div>", settings.latitude);
    sb_appendf(&page, "<div><label>经度<input name=\"longitude\" value=\"%.6f\"></label></div>", settings.longitude);
    sb_append(&page, "</div><div class=\"inline\">");
    sb_appendf(&page, "<div><label>后台绑定地址<input name=\"bind_addr\" value=\"%s\"></label></div>", settings.bind_addr);
    sb_appendf(&page, "<div><label>后台端口<input name=\"http_port\" value=\"%d\"></label><small>修改后重启生效</small></div>", settings.http_port);
    sb_append(&page, "</div><div class=\"inline\">");
    sb_appendf(&page, "<div><label>刷新间隔(分钟)<input name=\"refresh_interval_minutes\" value=\"%d\"></label></div>", settings.refresh_interval_minutes);
    sb_appendf(&page, "<div><label>本地相关半径(km)<input name=\"psk_radius_km\" value=\"%d\"></label></div>", settings.psk_radius_km);
    sb_append(&page, "</div><div class=\"inline\">");
    sb_appendf(&page, "<div><label>PSK窗口(分钟)<input name=\"psk_window_minutes\" value=\"%d\"></label></div>", settings.psk_window_minutes);
    sb_append(&page, "<div><label>管理员账号<input name=\"admin_user\" value=\"");
    html_escape_to_sb(&page, settings.admin_user);
    sb_append(&page, "\"></label></div></div><div><label>管理员密码<input name=\"admin_password\" value=\"");
    html_escape_to_sb(&page, settings.admin_password);
    sb_append(&page, "\"></label></div>"
        "<h2 style=\"margin-top:18px;\">OneBot / NapCat</h2><div><label>API Base URL<input name=\"onebot_api_base\" value=\"");
    html_escape_to_sb(&page, settings.onebot_api_base);
    sb_append(&page, "\"></label></div><div><label>Access Token<input name=\"onebot_access_token\" value=\"");
    html_escape_to_sb(&page, settings.onebot_access_token);
    sb_append(&page, "\"></label></div><div><label>Webhook Token<input name=\"onebot_webhook_token\" value=\"");
    html_escape_to_sb(&page, settings.onebot_webhook_token);
    sb_append(&page, "\"></label></div>"
        "<h2 style=\"margin-top:18px;\">定时推送</h2><div class=\"inline3\">");
    sb_appendf(&page, "<div><label>晨报时间<input name=\"schedule_morning\" value=\"%s\"></label></div>", settings.schedule_morning);
    sb_appendf(&page, "<div><label>晨报启用<select name=\"morning_enabled\"><option value=\"1\" %s>启用</option><option value=\"0\" %s>关闭</option></select></label></div>",
        settings.morning_enabled ? "selected" : "", settings.morning_enabled ? "" : "selected");
    sb_appendf(&page, "<div><label>晚报时间<input name=\"schedule_evening\" value=\"%s\"></label></div>", settings.schedule_evening);
    sb_appendf(&page, "<div><label>晚报启用<select name=\"evening_enabled\"><option value=\"1\" %s>启用</option><option value=\"0\" %s>关闭</option></select></label></div>",
        settings.evening_enabled ? "selected" : "", settings.evening_enabled ? "" : "selected");
    sb_append(&page, "</div><br><button type=\"submit\">保存设置</button></form></div>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"grid\" style=\"margin-top:18px;\">");
    sb_append(&page, "<div class=\"card\"><h2>推送目标</h2>"
        "<form method=\"post\" action=\"/targets/add\">"
        "<div class=\"inline3\">"
        "<div><label>名称<input name=\"label\"></label></div>"
        "<div><label>类型<select name=\"type\"><option value=\"group\">group</option><option value=\"private\">private</option></select></label></div>"
        "<div><label>目标 ID<input name=\"target_id\"></label></div></div>"
        "<div class=\"inline\"><div><label>备注<textarea name=\"notes\"></textarea></label></div>"
        "<div><label>命令回复<select name=\"command_enabled\"><option value=\"1\">yes</option><option value=\"0\">no</option></select></label>"
        "<label>启用<select name=\"enabled\"><option value=\"1\">enabled</option><option value=\"0\">disabled</option></select></label></div></div>"
        "<button type=\"submit\">添加目标</button></form>"
        "<table><thead><tr><th>Label</th><th>Type</th><th>ID</th><th>Status</th><th>Reply</th><th>Notes</th><th>Action</th></tr></thead><tbody>");
    sb_append(&page, targets_rows.data ? targets_rows.data : "");
    sb_append(&page, "</tbody></table></div>");

    sb_append(&page, "<div class=\"card\"><h2>最近本地相关 6m spot</h2><table><thead><tr><th>Time</th><th>Sender</th><th>Receiver</th><th>Mode</th><th>SNR</th><th>Distance</th></tr></thead><tbody>");
    sb_append(&page, recent_spots.data ? recent_spots.data : "");
    sb_append(&page, "</tbody></table></div>");
    sb_append(&page, "</div>");

    sb_append(&page, "<div class=\"card\" style=\"margin-top:18px;\"><h2>运行日志</h2><table><thead><tr><th>Time</th><th>Level</th><th>Message</th></tr></thead><tbody>");
    sb_append(&page, logs_rows.data ? logs_rows.data : "");
    sb_append(&page, "</tbody></table></div>");

    sb_append(&page, "</body></html>");

    sb_free(&recent_spots);
    sb_free(&logs_rows);
    sb_free(&targets_rows);
    return page.data;
}

static void save_settings_from_form(app_t *app, const char *body) {
    struct field_map {
        const char *form_key;
        const char *setting_key;
    } fields[] = {
        {"station_name", "station_name"},
        {"station_grid", "station_grid"},
        {"latitude", "latitude"},
        {"longitude", "longitude"},
        {"timezone", "timezone"},
        {"bind_addr", "bind_addr"},
        {"http_port", "http_port"},
        {"refresh_interval_minutes", "refresh_interval_minutes"},
        {"psk_radius_km", "psk_radius_km"},
        {"psk_window_minutes", "psk_window_minutes"},
        {"admin_user", "admin_user"},
        {"admin_password", "admin_password"},
        {"onebot_api_base", "onebot_api_base"},
        {"onebot_access_token", "onebot_access_token"},
        {"onebot_webhook_token", "onebot_webhook_token"},
        {"schedule_morning", "schedule_morning"},
        {"schedule_evening", "schedule_evening"},
        {"morning_enabled", "morning_enabled"},
        {"evening_enabled", "evening_enabled"},
    };
    char value[512];
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        if (get_form_value(body, fields[i].form_key, value, sizeof(value)) == 0) {
            storage_save_setting(app, fields[i].setting_key, value);
        }
    }
    storage_load_settings(app, &app->settings);
    apply_timezone(app->settings.timezone);
    app_log(app, "INFO", "后台配置已更新");
}

static void handle_add_target(app_t *app, const char *body) {
    target_t target;
    char value[512];
    memset(&target, 0, sizeof(target));
    get_form_value(body, "label", target.label, sizeof(target.label));
    get_form_value(body, "target_id", target.target_id, sizeof(target.target_id));
    get_form_value(body, "notes", target.notes, sizeof(target.notes));
    target.type = TARGET_GROUP;
    if (get_form_value(body, "type", value, sizeof(value)) == 0 && strcmp(value, "private") == 0) {
        target.type = TARGET_PRIVATE;
    }
    target.enabled = 1;
    if (get_form_value(body, "enabled", value, sizeof(value)) == 0) {
        target.enabled = atoi(value);
    }
    target.command_enabled = 1;
    if (get_form_value(body, "command_enabled", value, sizeof(value)) == 0) {
        target.command_enabled = atoi(value);
    }
    if (target.label[0] && target.target_id[0]) {
        storage_add_target(app, &target);
        app_log(app, "INFO", "新增推送目标: %s (%s)", target.label, target.target_id);
    }
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
    while (*p && (isdigit((unsigned char)*p) || *p == '-'
        || *p == '+' || *p == '.')
        && i + 1 < out_len) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0 ? 0 : -1;
}

static void handle_onebot_webhook(app_t *app, int fd, const http_request_t *req) {
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
    char raw_message[1024] = {0};
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

    target_type_t type = strcmp(message_type, "private") == 0 ? TARGET_PRIVATE : TARGET_GROUP;
    const char *target_id = type == TARGET_PRIVATE ? user_id : group_id;
    if (!target_id[0] || !target_can_reply(app, type, target_id)) {
        send_response(fd, "200 OK", "text/plain", "ignored");
        return;
    }

    refresh_snapshot(app, 0);
    pthread_mutex_lock(&app->cache_mutex);
    snapshot_t snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);

    const char *reply = NULL;
    if (string_contains_ci(raw_message, "help") || strstr(raw_message, "帮助")) {
        reply = "可用命令: 传播 / 6m / 太阳 / help";
    } else if (string_contains_ci(raw_message, "6m")) {
        reply = snapshot.report_6m;
    } else if (string_contains_ci(raw_message, "solar") || strstr(raw_message, "太阳")) {
        reply = snapshot.report_solar;
    } else if (string_contains_ci(raw_message, "prop") || strstr(raw_message, "传播")) {
        reply = snapshot.report_text;
    }

    if (reply) {
        onebot_send_message(app, type, target_id, reply);
    }
    send_response(fd, "200 OK", "text/plain", "ok");
}

static void handle_request(app_t *app, int fd, const http_request_t *req) {
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
        char id_text[64];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0) {
            storage_delete_target(app, atoi(id_text));
        }
        send_redirect(fd, "/");
        return;
    }

    if (strcmp(req->path, "/targets/toggle") == 0 && strcmp(req->method, "POST") == 0) {
        char id_text[64];
        char enabled_text[16];
        if (get_form_value(req->body, "id", id_text, sizeof(id_text)) == 0 &&
            get_form_value(req->body, "enabled", enabled_text, sizeof(enabled_text)) == 0) {
            storage_toggle_target(app, atoi(id_text), atoi(enabled_text));
        }
        send_redirect(fd, "/");
        return;
    }

    if (strcmp(req->path, "/actions/refresh") == 0 && strcmp(req->method, "POST") == 0) {
        refresh_snapshot(app, 1);
        send_redirect(fd, "/");
        return;
    }

    if (strcmp(req->path, "/actions/send") == 0 && strcmp(req->method, "POST") == 0) {
        char kind[32];
        refresh_snapshot(app, 0);
        pthread_mutex_lock(&app->cache_mutex);
        snapshot_t snapshot = app->snapshot;
        pthread_mutex_unlock(&app->cache_mutex);
        if (get_form_value(req->body, "kind", kind, sizeof(kind)) != 0) {
            kind[0] = '\0';
        }
        const char *report = snapshot.report_text;
        if (strcmp(kind, "6m") == 0) {
            report = snapshot.report_6m;
        } else if (strcmp(kind, "solar") == 0) {
            report = snapshot.report_solar;
        }
        send_report_to_all_targets(app, report);
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
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)app->settings.http_port);
    if (inet_pton(AF_INET, app->settings.bind_addr, &addr.sin_addr) != 1) {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        app_log(app, "ERROR", "HTTP bind failed on %s:%d", app->settings.bind_addr, app->settings.http_port);
        close(fd);
        return -1;
    }
    if (listen(fd, 32) != 0) {
        app_log(app, "ERROR", "HTTP listen failed");
        close(fd);
        return -1;
    }
    app->http_fd = fd;
    app_log(app, "INFO", "HTTP 后台已监听 %s:%d", app->settings.bind_addr, app->settings.http_port);

    while (app->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
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
    close(fd);
    return 0;
}
