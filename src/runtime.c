#include "app.h"

/* runtime.c 负责“把所有模块串起来”：
 * 1. 抓取外部数据
 * 2. 维护 snapshot 缓存
 * 3. 定时推送和事件告警
 * 4. 控制首页异步刷新状态
 */

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

static int onebot_extract_json_string(const char *json, const char *key, char *out, size_t out_len) {
    char pattern[128];
    if (!json || !key || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return -1;
    }
    p += strlen(pattern);
    size_t pos = 0;
    while (*p && *p != '"' && pos + 1 < out_len) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': out[pos++] = '\n'; break;
                case 'r': out[pos++] = '\r'; break;
                case 't': out[pos++] = '\t'; break;
                case '\\': out[pos++] = '\\'; break;
                case '"': out[pos++] = '"'; break;
                default: out[pos++] = *p; break;
            }
            p++;
            continue;
        }
        out[pos++] = *p++;
    }
    out[pos] = '\0';
    return 0;
}

static int onebot_extract_retcode(const char *json, long *out_retcode) {
    const char *p = json ? strstr(json, "\"retcode\":") : NULL;
    if (!p || !out_retcode) {
        return -1;
    }
    p += strlen("\"retcode\":");
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    *out_retcode = strtol(p, NULL, 10);
    return 0;
}

static int onebot_response_ok(const char *response, char *detail, size_t detail_len) {
    char status_text[64];
    char message_text[256];
    char wording_text[256];
    long retcode = 0;
    int has_retcode = 0;

    if (detail && detail_len > 0) {
        detail[0] = '\0';
    }
    if (!response) {
        if (detail && detail_len > 0) {
            copy_string(detail, detail_len, "OneBot 未返回响应");
        }
        return 0;
    }

    status_text[0] = '\0';
    message_text[0] = '\0';
    wording_text[0] = '\0';
    onebot_extract_json_string(response, "status", status_text, sizeof(status_text));
    onebot_extract_json_string(response, "message", message_text, sizeof(message_text));
    onebot_extract_json_string(response, "wording", wording_text, sizeof(wording_text));
    has_retcode = onebot_extract_retcode(response, &retcode) == 0;

    if (strcmp(status_text, "ok") == 0 && (!has_retcode || retcode == 0)) {
        return 1;
    }

    if (detail && detail_len > 0) {
        if (wording_text[0]) {
            copy_string(detail, detail_len, wording_text);
        } else if (message_text[0]) {
            copy_string(detail, detail_len, message_text);
        } else if (status_text[0]) {
            copy_string(detail, detail_len, status_text);
        } else {
            copy_string(detail, detail_len, "OneBot 返回失败");
        }
    }
    return 0;
}

static int message_has_remote_cq_image(const char *message) {
    return message &&
        (strstr(message, "[CQ:image,file=http://") != NULL ||
         strstr(message, "[CQ:image,file=https://") != NULL);
}

static char *message_replace_remote_cq_images_with_base64(const char *message, app_t *app) {
    sb_t out;
    int replaced = 0;
    const char *p = message;
    if (!message) {
        return NULL;
    }

    sb_init(&out);
    while (*p) {
        const char *tag = strstr(p, "[CQ:image,file=");
        if (!tag) {
            sb_append(&out, p);
            break;
        }

        if (tag > p) {
            size_t prefix_len = (size_t)(tag - p);
            char *prefix = malloc(prefix_len + 1);
            if (!prefix) {
                sb_free(&out);
                return NULL;
            }
            memcpy(prefix, p, prefix_len);
            prefix[prefix_len] = '\0';
            sb_append(&out, prefix);
            free(prefix);
        }

        const char *file = tag + strlen("[CQ:image,file=");
        const char *end = strchr(file, ']');
        if (!end) {
            sb_append(&out, tag);
            break;
        }

        size_t url_len = (size_t)(end - file);
        char url[1024];
        if (url_len >= sizeof(url)) {
            sb_free(&out);
            return NULL;
        }
        memcpy(url, file, url_len);
        url[url_len] = '\0';

        if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
            long status = 0;
            size_t image_size = 0;
            char *image_data = http_get_binary(url, NULL, &status, &image_size);
            if (!image_data || status < 200 || status >= 300 || image_size == 0) {
                if (app) {
                    app_log(app, "WARN", "远程图片下载失败，无法转成本地可发图片: %s status=%ld", url, status);
                }
                free(image_data);
                sb_free(&out);
                return NULL;
            }

            char *base64 = base64_encode_alloc((const unsigned char *)image_data, image_size);
            free(image_data);
            if (!base64) {
                sb_free(&out);
                return NULL;
            }

            sb_append(&out, "[CQ:image,file=base64://");
            sb_append(&out, base64);
            sb_append(&out, "]");
            free(base64);
            replaced = 1;
        } else {
            char original_tag[1200];
            snprintf(original_tag, sizeof(original_tag), "[CQ:image,file=%s]", url);
            sb_append(&out, original_tag);
        }

        p = end + 1;
    }

    if (!replaced) {
        sb_free(&out);
        return NULL;
    }
    return out.data;
}

static char *message_replace_remote_cq_images_with_links(const char *message) {
    if (!message) {
        return NULL;
    }

    size_t len = strlen(message);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }

    size_t out_pos = 0;
    const char *p = message;
    while (*p && out_pos < len) {
        const char *tag = strstr(p, "[CQ:image,file=");
        if (!tag) {
            size_t remaining = strlen(p);
            if (out_pos + remaining > len) {
                remaining = len - out_pos;
            }
            memcpy(out + out_pos, p, remaining);
            out_pos += remaining;
            break;
        }

        size_t prefix = (size_t)(tag - p);
        if (out_pos + prefix > len) {
            prefix = len - out_pos;
        }
        memcpy(out + out_pos, p, prefix);
        out_pos += prefix;

        const char *file = tag + strlen("[CQ:image,file=");
        const char *end = strchr(file, ']');
        if (!end) {
            size_t remaining = strlen(tag);
            if (out_pos + remaining > len) {
                remaining = len - out_pos;
            }
            memcpy(out + out_pos, tag, remaining);
            out_pos += remaining;
            break;
        }

        size_t url_len = (size_t)(end - file);
        if (url_len > 0) {
            if (out_pos + url_len > len) {
                url_len = len - out_pos;
            }
            memcpy(out + out_pos, file, url_len);
            out_pos += url_len;
        }
        p = end + 1;
    }

    out[out_pos] = '\0';
    return out;
}

static void build_onebot_message_json(sb_t *json, target_type_t type, const char *target_id, const char *message) {
    sb_append(json, "{");
    if (type == TARGET_PRIVATE) {
        if (looks_numeric(target_id)) {
            sb_appendf(json, "\"user_id\":%s,", target_id);
        } else {
            sb_append(json, "\"user_id\":\"");
            json_escape_to_sb(json, target_id);
            sb_append(json, "\",");
        }
    } else {
        if (looks_numeric(target_id)) {
            sb_appendf(json, "\"group_id\":%s,", target_id);
        } else {
            sb_append(json, "\"group_id\":\"");
            json_escape_to_sb(json, target_id);
            sb_append(json, "\",");
        }
    }
    sb_append(json, "\"message\":\"");
    json_escape_to_sb(json, message);
    sb_append(json, "\",\"auto_escape\":false}");
}

static int geomag_g_from_k_runtime(int kindex) {
    /* HAMqsl 给的是 K 指数，这里换算成更容易读的 G 级。 */
    if (kindex >= 9) return 5;
    if (kindex >= 8) return 4;
    if (kindex >= 7) return 3;
    if (kindex >= 6) return 2;
    if (kindex >= 5) return 1;
    return 0;
}

static int sixm_alert_level(const snapshot_t *snapshot, const settings_t *settings) {
    /* 6m 告警等级来自三类信息的组合：
     * 1. 本地 PSKReporter 实测
     * 2. F5LEN 对流层条件
     * 3. 天气侧面对 6m 的辅助评分
     *
     * PSK 实测优先级最高，因为它最接近“已经开口”的事实。 */
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
    /* 如果用户在运行中修改抓取间隔，这里会尽量平滑地调整下一次执行时间，
     * 而不是简单清零后马上全部重抓。 */
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

typedef struct {
    app_t *app;
    int force;
    int reset_schedule;
    char reason[MAX_TEXT];
} async_refresh_task_t;

void app_reset_poll_schedule(app_t *app, const settings_t *settings, time_t now) {
    /* 强制刷新完成后，把各轮询器的 next_due 推到“现在之后一个周期”，
     * 避免页面手动刷新后立刻又被周期线程重抓一遍。 */
    pthread_mutex_lock(&app->refresh_mutex);
    ensure_poll_state(&app->hamqsl_poll, settings->hamqsl_interval_minutes * 60, now);
    mark_poll_done(&app->hamqsl_poll, now);
    ensure_poll_state(&app->weather_poll, settings->weather_interval_minutes * 60, now);
    mark_poll_done(&app->weather_poll, now);
    ensure_poll_state(&app->tropo_poll, settings->tropo_interval_minutes * 60, now);
    mark_poll_done(&app->tropo_poll, now);
    ensure_poll_state(&app->meteor_poll, settings->meteor_interval_hours * 3600, now);
    mark_poll_done(&app->meteor_poll, now);
    ensure_poll_state(&app->satellite_poll, settings->satellite_interval_hours * 3600, now);
    mark_poll_done(&app->satellite_poll, now);
    ensure_poll_state(&app->psk_eval_poll, settings->psk_eval_interval_seconds, now);
    mark_poll_done(&app->psk_eval_poll, now);
    ensure_poll_state(&app->snapshot_poll, settings->snapshot_rebuild_seconds, now);
    mark_poll_done(&app->snapshot_poll, now);
    pthread_mutex_unlock(&app->refresh_mutex);
}

static void *async_refresh_thread(void *arg) {
    /* 后台页面加载时不再阻塞主线程抓数据，而是把重抓放到这个线程。 */
    async_refresh_task_t *task = (async_refresh_task_t *)arg;
    app_t *app = task->app;
    int force = task->force;
    int reset_schedule = task->reset_schedule;
    char reason[MAX_TEXT];
    copy_string(reason, sizeof(reason), task->reason);
    free(task);

    int rc = force ? app_force_refresh(app) : refresh_snapshot(app, 0);
    time_t now = time(NULL);

    if (reset_schedule && rc == 0) {
        settings_t settings;
        pthread_mutex_lock(&app->cache_mutex);
        settings = app->settings;
        pthread_mutex_unlock(&app->cache_mutex);
        app_reset_poll_schedule(app, &settings, now);
    }

    pthread_mutex_lock(&app->async_mutex);
    app->async_refresh_running = 0;
    app->async_refresh_last_rc = rc;
    app->async_refresh_finished_at = now;
    copy_string(app->async_refresh_status, sizeof(app->async_refresh_status), rc == 0 ? "空闲" : "失败");
    pthread_mutex_unlock(&app->async_mutex);

    app_log(app, rc == 0 ? "INFO" : "WARN", "异步刷新结束: reason=%s rc=%d", reason, rc);
    return NULL;
}

int app_request_refresh_async(app_t *app, int force, int reset_schedule, const char *reason) {
    /* 同一时刻只允许一个异步刷新任务，避免多个页面同时打开时重复抓网。 */
    async_refresh_task_t *task = calloc(1, sizeof(*task));
    char reason_copy[MAX_TEXT];
    copy_string(reason_copy, sizeof(reason_copy), reason && *reason ? reason : "manual");
    if (!task) {
        return -1;
    }

    pthread_mutex_lock(&app->async_mutex);
    if (app->async_refresh_running) {
        pthread_mutex_unlock(&app->async_mutex);
        free(task);
        return 1;
    }
    app->async_refresh_running = 1;
    app->async_refresh_last_rc = 0;
    app->async_refresh_started_at = time(NULL);
    copy_string(app->async_refresh_reason, sizeof(app->async_refresh_reason), reason_copy);
    copy_string(app->async_refresh_status, sizeof(app->async_refresh_status), "刷新中");
    pthread_mutex_unlock(&app->async_mutex);

    task->app = app;
    task->force = force;
    task->reset_schedule = reset_schedule;
    copy_string(task->reason, sizeof(task->reason), reason_copy);

    pthread_t thread;
    if (pthread_create(&thread, NULL, async_refresh_thread, task) != 0) {
        pthread_mutex_lock(&app->async_mutex);
        app->async_refresh_running = 0;
        app->async_refresh_last_rc = -1;
        app->async_refresh_finished_at = time(NULL);
        copy_string(app->async_refresh_status, sizeof(app->async_refresh_status), "启动失败");
        pthread_mutex_unlock(&app->async_mutex);
        free(task);
        return -1;
    }
    pthread_detach(thread);
    app_log(app, "INFO", "已排队异步刷新: reason=%s force=%d", reason_copy, force ? 1 : 0);
    return 0;
}

const char *app_get_report_by_kind(const snapshot_t *snapshot, const char *kind) {
    /* 后台按钮、定时任务、问答触发词最后都会走到这里，
     * 根据 kind 选择要发送哪一段报告。 */
    if (!kind || !*kind || strcmp(kind, "full") == 0) return snapshot->report_text;
    if (strcmp(kind, "6m") == 0) return snapshot->report_6m;
    if (strcmp(kind, "solar") == 0) return snapshot->report_solar;
    if (strcmp(kind, "geomag") == 0) return snapshot->report_geomag;
    if (strcmp(kind, "open6m") == 0) return snapshot->report_open6m;
    if (strcmp(kind, "help") == 0) return snapshot->report_help;
    return snapshot->report_text;
}

int onebot_send_message(app_t *app, target_type_t type, const char *target_id, const char *message) {
    /* 发送层尽量保持薄，只负责拼 OneBot JSON 和重试。 */
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
    build_onebot_message_json(&json, type, target_id, message);

    int attempts = settings.onebot_retry_count + 1;
    int ok = 0;
    long last_status = 0;
    char last_detail[256];
    char *fallback_message = NULL;
    last_detail[0] = '\0';
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        long status = 0;
        char *response = http_post_json(url, settings.onebot_access_token, json.data ? json.data : "{}", &status);
        ok = response != NULL && status >= 200 && status < 300 &&
            onebot_response_ok(response, last_detail, sizeof(last_detail));
        last_status = status;
        free(response);
        if (ok) {
            break;
        }
        if (attempt < attempts && settings.onebot_retry_delay_ms > 0) {
            app_sleep_ms(settings.onebot_retry_delay_ms);
        }
    }
    if (!ok && message_has_remote_cq_image(message)) {
        char *embedded_message = message_replace_remote_cq_images_with_base64(message, app);
        if (embedded_message && strcmp(embedded_message, message) != 0) {
            sb_t embedded_json;
            sb_init(&embedded_json);
            build_onebot_message_json(&embedded_json, type, target_id, embedded_message);

            long status = 0;
            char *response = http_post_json(url, settings.onebot_access_token,
                embedded_json.data ? embedded_json.data : "{}", &status);
            ok = response != NULL && status >= 200 && status < 300 &&
                onebot_response_ok(response, last_detail, sizeof(last_detail));
            last_status = status;
            if (ok) {
                app_log(app, "WARN",
                    "OneBot 远程图片已自动下载并转为可发送图片: target=%s",
                    target_id);
            }
            free(response);
            sb_free(&embedded_json);
        }
        free(embedded_message);
    }
    if (!ok && message_has_remote_cq_image(message)) {
        fallback_message = message_replace_remote_cq_images_with_links(message);
        if (fallback_message && strcmp(fallback_message, message) != 0) {
            sb_t fallback_json;
            sb_init(&fallback_json);
            build_onebot_message_json(&fallback_json, type, target_id, fallback_message);

            long status = 0;
            char *response = http_post_json(url, settings.onebot_access_token,
                fallback_json.data ? fallback_json.data : "{}", &status);
            ok = response != NULL && status >= 200 && status < 300 &&
                onebot_response_ok(response, last_detail, sizeof(last_detail));
            last_status = status;
            if (ok) {
                app_log(app, "WARN",
                    "OneBot 图片发送失败后已自动降级为文本链接: target=%s detail=%s",
                    target_id,
                    last_detail[0] ? last_detail : "remote image fallback");
            }
            free(response);
            sb_free(&fallback_json);
        }
    }
    if (!ok) {
        app_log(app, "ERROR", "OneBot 发送失败: target=%s status=%ld retries=%d",
            target_id, last_status, settings.onebot_retry_count);
    }
    if (!ok && last_detail[0]) {
        app_log(app, "ERROR", "OneBot 详细原因: target=%s detail=%s", target_id, last_detail);
    }
    free(fallback_message);
    sb_free(&json);
    return ok ? 0 : -1;
}

int send_report_to_all_targets(app_t *app, const char *message) {
    /* 群发会遵守发送间隔，尽量降低真人号场景下的风控压力。 */
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
    /* 发送前先让缓存进入“尽可能新”的状态。
     * 非 force 模式会优先走周期抓取逻辑，而不是每次都全量重抓。 */
    if (report_kind && strcmp(report_kind, "pskmap") == 0) {
        target_t targets[MAX_TARGETS];
        int count = 0;
        int sent = 0;
        int failed = 0;
        settings_t settings;

        pthread_mutex_lock(&app->cache_mutex);
        settings = app->settings;
        pthread_mutex_unlock(&app->cache_mutex);
        storage_load_targets(app, targets, MAX_TARGETS, &count);

        for (int i = 0; i < count; ++i) {
            if (!targets[i].enabled) {
                continue;
            }
            if (psk_send_snapshot_image(app, targets[i].type, targets[i].target_id) == 0) {
                sent++;
            } else {
                failed++;
            }
            if (i + 1 < count && settings.onebot_send_delay_ms > 0) {
                app_sleep_ms(settings.onebot_send_delay_ms);
            }
        }
        app_log(app, failed ? "WARN" : "INFO", "PSKReporter 快照群发完成: success=%d failed=%d", sent, failed);
        return sent > 0 ? sent : -1;
    }

    refresh_snapshot(app, 0);
    pthread_mutex_lock(&app->cache_mutex);
    snapshot_t snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);
    return send_report_to_all_targets(app, app_get_report_by_kind(&snapshot, report_kind));
}

void app_rebuild_snapshot(app_t *app) {
    /* rebuild 只重算“本地可得”的内容：
     * 例如 PSK 摘要、模板渲染、分析文本。
     * 它不会主动联网。 */
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
    /* force_refresh 是最重的刷新路径，会主动访问所有外部源。 */
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
    /* 如果异步刷新线程已经在工作，就让周期线程暂时让路，
     * 避免两个线程同时更新 snapshot。 */
    pthread_mutex_lock(&app->async_mutex);
    int async_running = app->async_refresh_running;
    pthread_mutex_unlock(&app->async_mutex);
    if (async_running) {
        return;
    }

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

    /* 各源独立轮询，互不绑死。
     * 例如天气接口慢了，不会影响 HAMqsl 或 PSK 的更新频率。 */
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
        /* 只要任意源有变化，或模板重建周期到了，就统一刷新最终 snapshot。 */
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
    /* 告警状态会落到数据库里，所以程序重启后不会丢失“上次已经发到哪一级”。 */
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
        /* 地磁告警只在等级“首次达到/继续升级”时发送一次。 */
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
        /* 6m 告警既支持“等级升级立即发”，也支持“同等级按最小间隔重复提醒”。 */
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
    /* 简单的每分钟计数器。
     * key 一般是 group:群号 或 private:QQ号。 */
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
    /* 对外统一的“刷新入口”。
     * force=1 直接全量抓取；force=0 走常规轮询 + 本地重建。 */
    if (force) {
        return app_force_refresh(app);
    }
    app_run_periodic_fetches(app);
    app_rebuild_snapshot(app);
    return 0;
}
