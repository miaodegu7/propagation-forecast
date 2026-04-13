#include "app.h"

static int get_kv_value(const char *text, const char *key, char *out, size_t out_len) {
    if (!text || !key || !out || out_len == 0) {
        return -1;
    }

    size_t key_len = strlen(key);
    const char *p = text;
    out[0] = '\0';
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) {
            break;
        }
        const char *amp = strchr(eq + 1, '&');
        size_t this_key_len = (size_t)(eq - p);
        if (this_key_len == key_len && strncmp(p, key, key_len) == 0) {
            size_t value_len = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            if (value_len >= out_len) {
                value_len = out_len - 1;
            }
            memcpy(out, eq + 1, value_len);
            out[value_len] = '\0';
            url_decode_inplace(out);
            return 0;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    return -1;
}

static int get_any_param(const char *query, const char *body,
                         const char *const *keys, size_t key_count,
                         char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    for (size_t i = 0; i < key_count; ++i) {
        if (query && get_kv_value(query, keys[i], out, out_len) == 0) {
            return 0;
        }
        if (body && get_kv_value(body, keys[i], out, out_len) == 0) {
            return 0;
        }
    }
    return -1;
}

static void append_unique_csv_local(char *csv, size_t csv_len, const char *value) {
    if (!csv || csv_len == 0 || !value || !*value || csv_contains_ci(csv, value)) {
        return;
    }
    if (csv[0]) {
        strncat(csv, ", ", csv_len - strlen(csv) - 1);
    }
    strncat(csv, value, csv_len - strlen(csv) - 1);
}

static int normalize_band_text(const char *input, char *out, size_t out_len) {
    if (!input || !*input || !out || out_len == 0) {
        return -1;
    }
    out[0] = '\0';
    if (string_contains_ci(input, "144") || string_contains_ci(input, "2m") || string_contains_ci(input, "2米")) {
        copy_string(out, out_len, "2m");
        return 0;
    }
    if (string_contains_ci(input, "50") || string_contains_ci(input, "6m") || string_contains_ci(input, "6米")) {
        copy_string(out, out_len, "6m");
        return 0;
    }
    return -1;
}

static int looks_like_grid(const char *text) {
    if (!text || strlen(text) < 4) {
        return 0;
    }
    return isalpha((unsigned char)text[0]) && isalpha((unsigned char)text[1]) &&
        isdigit((unsigned char)text[2]) && isdigit((unsigned char)text[3]);
}

static void uppercase_grid(char *text) {
    if (!text) {
        return;
    }
    for (char *p = text; *p; ++p) {
        *p = (char)toupper((unsigned char)*p);
    }
}

static int match_event_grid(const settings_t *settings, const char *event_grid,
                            char *matched_grid, size_t matched_grid_len,
                            char *matched_ol, size_t matched_ol_len) {
    char grids[MAX_HUGE_TEXT];
    if (!event_grid || !looks_like_grid(event_grid)) {
        return 0;
    }

    if (matched_grid && matched_grid_len > 0) {
        matched_grid[0] = '\0';
    }
    if (matched_ol && matched_ol_len > 0) {
        matched_ol[0] = '\0';
    }

    if (settings->psk_grids[0]) {
        copy_string(grids, sizeof(grids), settings->psk_grids);
    } else {
        copy_string(grids, sizeof(grids), settings->station_grid);
    }

    char event_upper[MAX_TEXT];
    copy_string(event_upper, sizeof(event_upper), event_grid);
    uppercase_grid(event_upper);

    char *save = NULL;
    for (char *part = strtok_r(grids, ",|/ \t\r\n", &save); part; part = strtok_r(NULL, ",|/ \t\r\n", &save)) {
        trim_whitespace(part);
        if (!*part) {
            continue;
        }
        uppercase_grid(part);
        if (prefix_matches_grid(event_upper, part)) {
            copy_string(matched_grid, matched_grid_len, part);
            if (strlen(event_upper) >= 4) {
                char ol[3] = {event_upper[2], event_upper[3], '\0'};
                copy_string(matched_ol, matched_ol_len, ol);
            }
            return 1;
        }
        if (settings->hamalert_match_ol &&
            strlen(event_upper) >= 4 && strlen(part) >= 4 &&
            isdigit((unsigned char)event_upper[2]) && isdigit((unsigned char)event_upper[3]) &&
            isdigit((unsigned char)part[2]) && isdigit((unsigned char)part[3]) &&
            event_upper[2] == part[2] && event_upper[3] == part[3]) {
            copy_string(matched_grid, matched_grid_len, part);
            {
                char ol[3] = {event_upper[2], event_upper[3], '\0'};
                copy_string(matched_ol, matched_ol_len, ol);
            }
            return 1;
        }
    }
    return 0;
}

static void add_hamalert_event(app_t *app, const hamalert_event_t *event) {
    pthread_mutex_lock(&app->hamalert_mutex);
    app->hamalert_events[app->hamalert_head] = *event;
    app->hamalert_events[app->hamalert_head].in_use = 1;
    app->hamalert_head = (app->hamalert_head + 1) % MAX_HAMALERT_EVENTS;
    if (app->hamalert_count < MAX_HAMALERT_EVENTS) {
        app->hamalert_count++;
    }
    pthread_mutex_unlock(&app->hamalert_mutex);
}

static void format_event_text(const hamalert_event_t *event, char *out, size_t out_len) {
    char locator_text[MAX_TEXT] = "";
    if (event->locator[0]) {
        copy_string(locator_text, sizeof(locator_text), event->locator);
    } else if (event->dx_locator[0]) {
        copy_string(locator_text, sizeof(locator_text), event->dx_locator);
    } else if (event->spotter_locator[0]) {
        copy_string(locator_text, sizeof(locator_text), event->spotter_locator);
    }

    snprintf(out, out_len, "%s %s %s %s %s",
        event->source[0] ? event->source : "HamAlert",
        event->callsign[0] ? event->callsign : "-",
        event->spotter_call[0] ? event->spotter_call : "-",
        locator_text[0] ? locator_text : "-",
        event->mode[0] ? event->mode : "-");
    trim_whitespace(out);
}

static void maybe_upgrade_summary_from_hamalert(const settings_t *settings, psk_summary_t *summary) {
    if (!settings || !summary) {
        return;
    }
    if (summary->hamalert_hits_15m >= 2) {
        if (summary->score < 75) {
            summary->score = 75;
        }
        if (summary->local_spots_60m == 0) {
            copy_string(summary->assessment, sizeof(summary->assessment),
                settings->wording_psk_assessment_hamalert_open[0] ? settings->wording_psk_assessment_hamalert_open : "HamAlert 有相关开口报告");
            copy_string(summary->confidence, sizeof(summary->confidence),
                settings->wording_psk_confidence_medium[0] ? settings->wording_psk_confidence_medium : "中");
        }
    } else if (summary->hamalert_hits_60m > 0) {
        if (summary->score < 55) {
            summary->score = 55;
        }
        if (summary->local_spots_60m == 0) {
            copy_string(summary->assessment, sizeof(summary->assessment),
                settings->wording_psk_assessment_hamalert_hint[0] ? settings->wording_psk_assessment_hamalert_hint : "HamAlert 有相关线索");
            copy_string(summary->confidence, sizeof(summary->confidence),
                settings->wording_psk_confidence_low[0] ? settings->wording_psk_confidence_low : "低");
        }
    }
}

int hamalert_handle_webhook(app_t *app, const char *query, const char *body, const char *content_type,
                            char *response, size_t response_len) {
    static const char *token_keys[] = {"token", "auth", "key"};
    static const char *band_keys[] = {"band", "Band", "frequency", "freq", "mhz", "title", "text", "comment"};
    static const char *source_keys[] = {"source", "src"};
    static const char *call_keys[] = {"call", "callsign", "dx_call", "dx"};
    static const char *spotter_keys[] = {"spotter", "spotter_call", "de", "reporter"};
    static const char *mode_keys[] = {"mode", "Mode"};
    static const char *locator_keys[] = {"locator", "grid", "loc", "gridsquare"};
    static const char *dx_locator_keys[] = {"dx_locator", "dx_grid", "dx_loc"};
    static const char *spotter_locator_keys[] = {"spotter_locator", "spotter_grid", "spotter_loc"};
    static const char *comment_keys[] = {"comment", "message", "text", "title"};
    settings_t settings;
    char token[MAX_LARGE_TEXT] = "";
    char band_text[MAX_TEXT] = "";
    hamalert_event_t event;

    (void)content_type;

    pthread_mutex_lock(&app->cache_mutex);
    settings = app->settings;
    pthread_mutex_unlock(&app->cache_mutex);

    if (!settings.hamalert_enabled) {
        snprintf(response, response_len, "{\"ok\":false,\"reason\":\"disabled\"}");
        return 403;
    }

    get_any_param(query, body, token_keys, sizeof(token_keys) / sizeof(token_keys[0]), token, sizeof(token));
    if (settings.hamalert_webhook_token[0] && strcmp(token, settings.hamalert_webhook_token) != 0) {
        snprintf(response, response_len, "{\"ok\":false,\"reason\":\"token\"}");
        return 403;
    }

    memset(&event, 0, sizeof(event));
    get_any_param(query, body, source_keys, sizeof(source_keys) / sizeof(source_keys[0]), event.source, sizeof(event.source));
    get_any_param(query, body, call_keys, sizeof(call_keys) / sizeof(call_keys[0]), event.callsign, sizeof(event.callsign));
    get_any_param(query, body, spotter_keys, sizeof(spotter_keys) / sizeof(spotter_keys[0]), event.spotter_call, sizeof(event.spotter_call));
    get_any_param(query, body, mode_keys, sizeof(mode_keys) / sizeof(mode_keys[0]), event.mode, sizeof(event.mode));
    get_any_param(query, body, locator_keys, sizeof(locator_keys) / sizeof(locator_keys[0]), event.locator, sizeof(event.locator));
    get_any_param(query, body, dx_locator_keys, sizeof(dx_locator_keys) / sizeof(dx_locator_keys[0]), event.dx_locator, sizeof(event.dx_locator));
    get_any_param(query, body, spotter_locator_keys, sizeof(spotter_locator_keys) / sizeof(spotter_locator_keys[0]), event.spotter_locator, sizeof(event.spotter_locator));
    get_any_param(query, body, comment_keys, sizeof(comment_keys) / sizeof(comment_keys[0]), event.comment, sizeof(event.comment));
    get_any_param(query, body, band_keys, sizeof(band_keys) / sizeof(band_keys[0]), band_text, sizeof(band_text));

    uppercase_grid(event.locator);
    uppercase_grid(event.dx_locator);
    uppercase_grid(event.spotter_locator);

    if (normalize_band_text(band_text, event.band, sizeof(event.band)) != 0) {
        snprintf(response, response_len, "{\"ok\":true,\"reason\":\"ignored\"}");
        return 202;
    }
    if (!event.source[0]) {
        copy_string(event.source, sizeof(event.source), "HamAlert");
    }
    event.timestamp = time(NULL);
    add_hamalert_event(app, &event);

    app_log(app, "INFO", "HamAlert 已接收 %s 事件: call=%s locator=%s spotter=%s",
        event.band,
        event.callsign[0] ? event.callsign : "-",
        event.locator[0] ? event.locator : (event.dx_locator[0] ? event.dx_locator : "-"),
        event.spotter_call[0] ? event.spotter_call : "-");

    snprintf(response, response_len, "{\"ok\":true,\"band\":\"%s\"}", event.band);
    return 200;
}

void hamalert_apply_to_summary(app_t *app, const settings_t *settings, const char *band, psk_summary_t *summary) {
    time_t now;
    time_t cutoff_15;
    time_t cutoff_60;
    time_t latest_ts = 0;

    if (!app || !settings || !band || !summary) {
        return;
    }

    summary->hamalert_hits_15m = 0;
    summary->hamalert_hits_60m = 0;
    summary->hamalert_latest_time[0] = '\0';
    summary->hamalert_latest_text[0] = '\0';
    summary->hamalert_sources[0] = '\0';
    summary->hamalert_matched_grids[0] = '\0';
    summary->hamalert_matched_ols[0] = '\0';

    if (!settings->hamalert_enabled) {
        return;
    }
    if (strcmp(band, "6m") == 0 && !settings->hamalert_use_for_6m) {
        return;
    }
    if (strcmp(band, "2m") == 0 && !settings->hamalert_use_for_2m) {
        return;
    }

    now = time(NULL);
    cutoff_15 = now - 15 * 60;
    cutoff_60 = now - settings->psk_window_minutes * 60;

    pthread_mutex_lock(&app->hamalert_mutex);
    for (size_t i = 0; i < app->hamalert_count; ++i) {
        const hamalert_event_t *event = &app->hamalert_events[i];
        char matched_grid[MAX_TEXT] = "";
        char matched_ol[16] = "";
        char locator_to_check[MAX_TEXT] = "";
        int matched = 0;

        if (!event->in_use || event->timestamp < cutoff_60 || strcasecmp(event->band, band) != 0) {
            continue;
        }

        if (event->locator[0]) {
            copy_string(locator_to_check, sizeof(locator_to_check), event->locator);
            matched = match_event_grid(settings, locator_to_check, matched_grid, sizeof(matched_grid), matched_ol, sizeof(matched_ol));
        }
        if (!matched && event->dx_locator[0]) {
            copy_string(locator_to_check, sizeof(locator_to_check), event->dx_locator);
            matched = match_event_grid(settings, locator_to_check, matched_grid, sizeof(matched_grid), matched_ol, sizeof(matched_ol));
        }
        if (!matched && event->spotter_locator[0]) {
            copy_string(locator_to_check, sizeof(locator_to_check), event->spotter_locator);
            matched = match_event_grid(settings, locator_to_check, matched_grid, sizeof(matched_grid), matched_ol, sizeof(matched_ol));
        }
        if (!matched) {
            continue;
        }

        summary->hamalert_hits_60m++;
        if (event->timestamp >= cutoff_15) {
            summary->hamalert_hits_15m++;
        }
        append_unique_csv_local(summary->hamalert_sources, sizeof(summary->hamalert_sources), event->source);
        append_unique_csv_local(summary->hamalert_matched_grids, sizeof(summary->hamalert_matched_grids), matched_grid);
        append_unique_csv_local(summary->hamalert_matched_ols, sizeof(summary->hamalert_matched_ols), matched_ol);
        append_unique_csv_local(summary->matched_grids, sizeof(summary->matched_grids), matched_grid);

        if (event->timestamp >= latest_ts) {
            char latest_text[MAX_LARGE_TEXT];
            latest_ts = event->timestamp;
            format_time_local(event->timestamp, summary->hamalert_latest_time, sizeof(summary->hamalert_latest_time));
            format_event_text(event, latest_text, sizeof(latest_text));
            copy_string(summary->hamalert_latest_text, sizeof(summary->hamalert_latest_text), latest_text);
        }
    }
    pthread_mutex_unlock(&app->hamalert_mutex);

    if (summary->hamalert_hits_15m > 0 || summary->hamalert_hits_60m > 0) {
        int boost = clamp_int(summary->hamalert_hits_15m * 16 + summary->hamalert_hits_60m * 6, 0, 35);
        summary->score = clamp_int(summary->score + boost, 0, 100);
        maybe_upgrade_summary_from_hamalert(settings, summary);
    }
}
