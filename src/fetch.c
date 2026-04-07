#include "app.h"
#include <limits.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"

typedef struct {
    int hours;
    char url[MAX_LARGE_TEXT];
} tropo_image_t;

typedef struct {
    const char *name;
    const char *value;
} template_token_t;

typedef struct {
    int r;
    int g;
    int b;
    const char *label;
    int score;
} tropo_palette_t;

typedef struct {
    char name[MAX_TEXT];
    char peak_label[MAX_TEXT];
    int moon_percent;
    int days_left;
} meteor_candidate_t;

static int extract_tag_text(const char *xml, const char *tag, char *out, size_t out_len) {
    char open_tag[64];
    char close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    const char *start = strstr(xml, open_tag);
    if (!start) {
        return -1;
    }
    start += strlen(open_tag);
    const char *end = strstr(start, close_tag);
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

static const char *find_json_key(const char *json, const char *key) {
    static char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static int copy_balanced_json(const char *start, char open_ch, char close_ch, char *out, size_t out_len) {
    if (!start || *start != open_ch) {
        return -1;
    }
    int depth = 0;
    int in_string = 0;
    const char *p = start;
    while (*p) {
        if (*p == '"' && (p == start || p[-1] != '\\')) {
            in_string = !in_string;
        } else if (!in_string) {
            if (*p == open_ch) {
                depth++;
            } else if (*p == close_ch) {
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
        }
        p++;
    }
    return -1;
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
    return copy_balanced_json(p, '{', '}', out, out_len);
}

static int extract_json_array(const char *json, const char *key, char *out, size_t out_len) {
    const char *p = find_json_key(json, key);
    if (!p) {
        return -1;
    }
    p = strchr(p, ':');
    if (!p) {
        return -1;
    }
    while (*p && *p != '[') {
        p++;
    }
    return copy_balanced_json(p, '[', ']', out, out_len);
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
    return end != p ? 0 : -1;
}

static int extract_json_first_array_string(const char *json, const char *key, char *out, size_t out_len) {
    char array_text[8192];
    if (extract_json_array(json, key, array_text, sizeof(array_text)) != 0) {
        return -1;
    }
    const char *p = strchr(array_text, '"');
    if (!p) {
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
    char array_text[8192];
    if (extract_json_array(json, key, array_text, sizeof(array_text)) != 0) {
        return -1;
    }
    const char *p = strchr(array_text, '[');
    if (!p) {
        return -1;
    }
    p++;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    char *end = NULL;
    *out = strtod(p, &end);
    return end != p ? 0 : -1;
}

static int parse_month_abbr(const char *text) {
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    for (int i = 0; i < 12; ++i) {
        if (strncasecmp(text, months[i], 3) == 0) {
            return i;
        }
    }
    return -1;
}

static int parse_peak_date(const char *text, struct tm *tm_out) {
    char month_text[8];
    int day = 0;
    int year = 0;
    if (sscanf(text, "%7s %d %d", month_text, &day, &year) != 3) {
        return -1;
    }
    int month = parse_month_abbr(month_text);
    if (month < 0) {
        return -1;
    }
    memset(tm_out, 0, sizeof(*tm_out));
    tm_out->tm_year = year - 1900;
    tm_out->tm_mon = month;
    tm_out->tm_mday = day;
    tm_out->tm_hour = 12;
    return 0;
}

static void strip_tags_inplace(char *text) {
    char *src = text;
    char *dst = text;
    int in_tag = 0;
    while (*src) {
        if (*src == '<') {
            in_tag = 1;
        } else if (*src == '>') {
            in_tag = 0;
            src++;
            continue;
        } else if (!in_tag) {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';
    trim_whitespace(text);
}

static int parse_calendar_peak_date(const char *text, struct tm *tm_out) {
    char month_text[8];
    int day1 = 0;
    int day2 = 0;
    int year = 0;
    int day = 0;

    if (sscanf(text, "%7s %d-%d, %d", month_text, &day1, &day2, &year) == 4 ||
        sscanf(text, "%7s %d-%d %d", month_text, &day1, &day2, &year) == 4) {
        day = day2;
    } else if (sscanf(text, "%7s %d, %d", month_text, &day1, &year) == 3 ||
               sscanf(text, "%7s %d %d", month_text, &day1, &year) == 3) {
        day = day1;
    } else {
        return -1;
    }

    int month = parse_month_abbr(month_text);
    if (month < 0) {
        return -1;
    }

    memset(tm_out, 0, sizeof(*tm_out));
    tm_out->tm_year = year - 1900;
    tm_out->tm_mon = month;
    tm_out->tm_mday = day;
    tm_out->tm_hour = 12;
    return 0;
}

static int meteor_days_left_from_peak(const char *peak_label) {
    struct tm peak_tm;
    if (parse_calendar_peak_date(peak_label, &peak_tm) != 0 &&
        parse_peak_date(peak_label, &peak_tm) != 0) {
        return -1;
    }
    time_t peak_time = mktime(&peak_tm);
    time_t now = time(NULL);
    return (int)floor(difftime(peak_time, now) / 86400.0 + 0.5);
}

static int meteor_matches_filter(const char *name, const char *filter_csv) {
    if (!filter_csv || !*filter_csv) {
        return 1;
    }
    char temp[MAX_HUGE_TEXT];
    copy_string(temp, sizeof(temp), filter_csv);
    char *save = NULL;
    for (char *part = strtok_r(temp, ",|/\t\r\n", &save); part; part = strtok_r(NULL, ",|/\t\r\n", &save)) {
        trim_whitespace(part);
        if (*part && string_contains_ci(name, part)) {
            return 1;
        }
    }
    return 0;
}

static int cmp_meteor_candidates(const void *a, const void *b) {
    const meteor_candidate_t *ma = (const meteor_candidate_t *)a;
    const meteor_candidate_t *mb = (const meteor_candidate_t *)b;
    int da = ma->days_left < 0 ? INT_MAX / 2 : ma->days_left;
    int db = mb->days_left < 0 ? INT_MAX / 2 : mb->days_left;
    if (da != db) {
        return da - db;
    }
    return strcasecmp(ma->name, mb->name);
}

static int parse_meteor_calendar_candidates(const char *html, meteor_candidate_t *items, int max_items) {
    int count = 0;
    const char *p = html;
    const char *marker = "<div class=\"shower media\"";
    while ((p = strstr(p, marker)) != NULL && count < max_items) {
        const char *next = strstr(p + strlen(marker), marker);
        size_t block_len = next ? (size_t)(next - p) : strlen(p);
        if (block_len >= 32768) {
            block_len = 32767;
        }
        char block[32768];
        memcpy(block, p, block_len);
        block[block_len] = '\0';

        meteor_candidate_t item;
        memset(&item, 0, sizeof(item));

        const char *name = strstr(block, "class=\"media-heading\"");
        if (name) {
            name = strchr(name, '>');
            if (name) {
                const char *end = strchr(name + 1, '<');
                if (end && end > name + 1) {
                    size_t len = (size_t)(end - (name + 1));
                    if (len >= sizeof(item.name)) {
                        len = sizeof(item.name) - 1;
                    }
                    memcpy(item.name, name + 1, len);
                    item.name[len] = '\0';
                    strip_tags_inplace(item.name);
                    char *paren = strchr(item.name, '(');
                    if (paren) {
                        *paren = '\0';
                        trim_whitespace(item.name);
                    }
                }
            }
        }

        const char *peak = strstr(block, "will next peak on the ");
        if (peak) {
            peak += strlen("will next peak on the ");
            const char *end = strstr(peak, " night");
            if (!end) {
                end = strchr(peak, '.');
            }
            if (end && end > peak) {
                size_t len = (size_t)(end - peak);
                if (len >= sizeof(item.peak_label)) {
                    len = sizeof(item.peak_label) - 1;
                }
                memcpy(item.peak_label, peak, len);
                item.peak_label[len] = '\0';
                trim_whitespace(item.peak_label);
            }
        }

        const char *moon = strstr(block, "moon will be ");
        if (moon) {
            moon += strlen("moon will be ");
            item.moon_percent = atoi(moon);
        }

        if (item.name[0] && item.peak_label[0]) {
            item.days_left = meteor_days_left_from_peak(item.peak_label);
            items[count++] = item;
        }

        if (!next) {
            break;
        }
        p = next;
    }
    return count;
}

static int parse_meteor_widget_candidate(const char *html, meteor_candidate_t *item) {
    const char *widget = strstr(html, "widget meteor-shower-widget");
    if (!widget || !item) {
        return -1;
    }

    memset(item, 0, sizeof(*item));

    const char *name_a = strstr(widget, "class=\"shower_t\"");
    if (name_a) {
        name_a = strchr(name_a, '>');
        if (name_a) {
            const char *name_end = strstr(name_a + 1, "</a>");
            if (name_end && name_end > name_a + 1) {
                size_t len = (size_t)(name_end - (name_a + 1));
                if (len >= sizeof(item->name)) {
                    len = sizeof(item->name) - 1;
                }
                memcpy(item->name, name_a + 1, len);
                item->name[len] = '\0';
                strip_tags_inplace(item->name);
            }
        }
    }

    const char *peak = strstr(widget, "Peak Night:</strong>");
    if (peak) {
        peak += strlen("Peak Night:</strong>");
        while (*peak && isspace((unsigned char)*peak)) {
            peak++;
        }
        const char *end = strchr(peak, '.');
        if (end && end > peak) {
            size_t len = (size_t)(end - peak);
            if (len >= sizeof(item->peak_label)) {
                len = sizeof(item->peak_label) - 1;
            }
            memcpy(item->peak_label, peak, len);
            item->peak_label[len] = '\0';
            trim_whitespace(item->peak_label);
        }
    }

    const char *moon = strstr(widget, "moon will be <strong>");
    if (moon) {
        moon += strlen("moon will be <strong>");
        item->moon_percent = atoi(moon);
    }

    if (!item->name[0] || !item->peak_label[0]) {
        return -1;
    }
    item->days_left = meteor_days_left_from_peak(item->peak_label);
    return 0;
}

static int ham_field_enabled(const settings_t *settings, const char *field_name) {
    return !settings->hamqsl_selected_fields[0] || csv_contains_ci(settings->hamqsl_selected_fields, field_name);
}

static void append_key_value(sb_t *sb, const char *label, const char *value) {
    if (!value || !*value) {
        return;
    }
    if (sb->len > 0 && sb->data[sb->len - 1] != '\n') {
        sb_append(sb, "\n");
    }
    sb_appendf(sb, "%s：%s", label, value);
}

static void append_key_value_int(sb_t *sb, const char *label, int value) {
    if (sb->len > 0 && sb->data[sb->len - 1] != '\n') {
        sb_append(sb, "\n");
    }
    sb_appendf(sb, "%s：%d", label, value);
}

static void append_key_value_double(sb_t *sb, const char *label, double value, const char *unit, int precision) {
    char fmt[32];
    snprintf(fmt, sizeof(fmt), "%%s：%%.%df%%s", precision);
    if (sb->len > 0 && sb->data[sb->len - 1] != '\n') {
        sb_append(sb, "\n");
    }
    sb_appendf(sb, fmt, label, value, unit ? unit : "");
}

static int geomag_g_from_k(int kindex) {
    if (kindex >= 9) return 5;
    if (kindex >= 8) return 4;
    if (kindex >= 7) return 3;
    if (kindex >= 6) return 2;
    if (kindex >= 5) return 1;
    return 0;
}

static const char *sixm_alert_label_from_snapshot(const snapshot_t *snapshot, const settings_t *settings) {
    int psk_hot = snapshot->psk.local_spots_15m >= settings->sixm_psk_trigger_spots ||
        snapshot->psk.local_spots_60m >= settings->sixm_psk_trigger_spots;
    int psk_some = snapshot->psk.local_spots_60m > 0;
    int tropo_hot = snapshot->tropo.valid && snapshot->tropo.score >= 78;
    int tropo_some = snapshot->tropo.valid && snapshot->tropo.score >= 55;
    int weather_hot = snapshot->weather.valid && snapshot->weather.sixm_weather_score >= 70;
    int weather_some = snapshot->weather.valid && snapshot->weather.sixm_weather_score >= 50;

    if (psk_hot && (tropo_some || weather_some)) return "强提醒";
    if (psk_hot || (psk_some && (tropo_some || weather_some)) || (tropo_hot && weather_hot)) return "重点观察";
    if (psk_some || tropo_some || weather_some) return "一般提醒";
    return "暂无提醒";
}

static int render_template(char *out, size_t out_len, const char *tmpl,
                           const template_token_t *tokens, size_t token_count) {
    sb_t sb;
    sb_init(&sb);
    const char *p = tmpl ? tmpl : "";
    while (*p) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (end) {
                char name[128];
                size_t len = (size_t)(end - (p + 2));
                if (len >= sizeof(name)) len = sizeof(name) - 1;
                memcpy(name, p + 2, len);
                name[len] = '\0';
                trim_whitespace(name);
                const char *value = "";
                for (size_t i = 0; i < token_count; ++i) {
                    if (strcmp(tokens[i].name, name) == 0) {
                        value = tokens[i].value ? tokens[i].value : "";
                        break;
                    }
                }
                sb_append(&sb, value);
                p = end + 2;
                continue;
            }
        }
        char ch[2] = {*p++, '\0'};
        sb_append(&sb, ch);
    }
    copy_string(out, out_len, sb.data ? sb.data : "");
    sb_free(&sb);
    return 0;
}

static void append_band_summary(sb_t *sb, const hamqsl_data_t *ham, const char *time_slot, const char *label) {
    int emitted = 0;
    for (int i = 0; i < ham->band_count; ++i) {
        if (strcasecmp(ham->bands[i].time_slot, time_slot) != 0) {
            continue;
        }
        if (!emitted) {
            if (sb->len > 0) sb_append(sb, "\n");
            sb_appendf(sb, "%s：", label);
        } else {
            sb_append(sb, " | ");
        }
        sb_appendf(sb, "%s %s", ham->bands[i].name, ham->bands[i].status);
        emitted++;
    }
}

static int parse_tropo_hours_from_url(const char *url) {
    const char *dot = strrchr(url, '.');
    if (!dot) dot = url + strlen(url);
    const char *p = dot;
    while (p > url && isdigit((unsigned char)p[-1])) {
        p--;
    }
    return p < dot ? atoi(p) : -1;
}

static int parse_tropo_images(const char *html, tropo_image_t *images, int max_images) {
    int count = 0;
    const char *p = html;
    while ((p = strstr(p, "imgArray[")) != NULL && count < max_images) {
        const char *q1 = strchr(p, '"');
        if (!q1) break;
        const char *q2 = strchr(q1 + 1, '"');
        if (!q2) break;
        size_t len = (size_t)(q2 - (q1 + 1));
        if (len >= sizeof(images[count].url)) len = sizeof(images[count].url) - 1;
        memcpy(images[count].url, q1 + 1, len);
        images[count].url[len] = '\0';
        images[count].hours = parse_tropo_hours_from_url(images[count].url);
        count++;
        p = q2 + 1;
    }
    return count;
}

static void classify_tropo_pixel(int r, int g, int b, char *label, size_t label_len, int *score) {
    static const tropo_palette_t palette[] = {
        {210, 210, 254, "普通", 25},
        {170, 170, 170, "受扰", 10},
        {160, 0, 200, "轻微增强", 38},
        {130, 0, 220, "弱增强", 50},
        {0, 220, 0, "良好", 65},
        {230, 220, 50, "较强", 78},
        {240, 130, 40, "很强", 90},
        {250, 60, 60, "波导", 100}
    };
    long best_dist = LONG_MAX;
    int best_index = 0;
    for (size_t i = 0; i < sizeof(palette) / sizeof(palette[0]); ++i) {
        long dr = (long)r - palette[i].r;
        long dg = (long)g - palette[i].g;
        long db = (long)b - palette[i].b;
        long dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best_index = (int)i;
        }
    }
    copy_string(label, label_len, palette[best_index].label);
    if (score) *score = palette[best_index].score;
}

static void sample_png_average(const unsigned char *img, int width, int height, int channels,
                               int cx, int cy, int *r, int *g, int *b) {
    long sum_r = 0;
    long sum_g = 0;
    long sum_b = 0;
    int count = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int x = clamp_int(cx + dx, 0, width - 1);
            int y = clamp_int(cy + dy, 0, height - 1);
            const unsigned char *px = img + ((size_t)y * (size_t)width + (size_t)x) * (size_t)channels;
            sum_r += px[0];
            sum_g += px[1];
            sum_b += px[2];
            count++;
        }
    }
    *r = count ? (int)(sum_r / count) : 0;
    *g = count ? (int)(sum_g / count) : 0;
    *b = count ? (int)(sum_b / count) : 0;
}

static int time_in_window_local(time_t when, const char *start_hhmm, const char *end_hhmm) {
    int start = parse_hhmm(start_hhmm);
    int end = parse_hhmm(end_hhmm);
    if (start < 0 || end < 0) return 1;
    struct tm tm_local;
    localtime_r(&when, &tm_local);
    int minute = tm_local.tm_hour * 60 + tm_local.tm_min;
    if (start <= end) return minute >= start && minute <= end;
    return minute >= start || minute <= end;
}

static int satellite_mode_allowed(const satellite_t *sat, const settings_t *settings) {
    if (!settings->satellite_mode_filter[0] ||
        strcmp(settings->satellite_mode_filter, "全部") == 0 ||
        strcasecmp(settings->satellite_mode_filter, "all") == 0) {
        return 1;
    }
    return strcasecmp(sat->mode_type, settings->satellite_mode_filter) == 0;
}

static int cmp_passes(const void *a, const void *b) {
    const satellite_pass_t *pa = (const satellite_pass_t *)a;
    const satellite_pass_t *pb = (const satellite_pass_t *)b;
    return strcmp(pa->start_local, pb->start_local);
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
    const char *source = strstr(xml, "<source");
    if (source) {
        extract_attr(source, "url", out->source_url, sizeof(out->source_url));
        const char *tag_end = strchr(source, '>');
        const char *close = strstr(source, "</source>");
        if (tag_end && close && close > tag_end) {
            size_t len = (size_t)(close - (tag_end + 1));
            if (len >= sizeof(out->source_name)) len = sizeof(out->source_name) - 1;
            memcpy(out->source_name, tag_end + 1, len);
            out->source_name[len] = '\0';
            trim_whitespace(out->source_name);
        }
    }
    extract_tag_text(xml, "solarflux", temp, sizeof(temp)); out->solarflux = atoi(temp);
    extract_tag_text(xml, "aindex", temp, sizeof(temp)); out->aindex = atoi(temp);
    extract_tag_text(xml, "kindex", temp, sizeof(temp)); out->kindex = atoi(temp);
    extract_tag_text(xml, "kindexnt", out->kindex_text, sizeof(out->kindex_text));
    extract_tag_text(xml, "xray", out->xray, sizeof(out->xray));
    extract_tag_text(xml, "sunspots", temp, sizeof(temp)); out->sunspots = atoi(temp);
    extract_tag_text(xml, "heliumline", temp, sizeof(temp)); out->heliumline = atof(temp);
    extract_tag_text(xml, "protonflux", temp, sizeof(temp)); out->protonflux = atoi(temp);
    extract_tag_text(xml, "electonflux", temp, sizeof(temp)); out->electronflux = atoi(temp);
    extract_tag_text(xml, "aurora", temp, sizeof(temp)); out->aurora = atoi(temp);
    extract_tag_text(xml, "normalization", temp, sizeof(temp)); out->normalization = atof(temp);
    extract_tag_text(xml, "latdegree", temp, sizeof(temp)); out->latdegree = atof(temp);
    extract_tag_text(xml, "solarwind", temp, sizeof(temp)); out->solarwind = atof(temp);
    extract_tag_text(xml, "magneticfield", temp, sizeof(temp)); out->magneticfield = atof(temp);
    extract_tag_text(xml, "geomagfield", out->geomagfield, sizeof(out->geomagfield));
    extract_tag_text(xml, "signalnoise", out->signalnoise, sizeof(out->signalnoise));
    extract_tag_text(xml, "fof2", out->fof2, sizeof(out->fof2));
    extract_tag_text(xml, "muffactor", out->muffactor, sizeof(out->muffactor));
    extract_tag_text(xml, "muf", out->muf, sizeof(out->muf));
    parse_conditions(xml, out);
    out->valid = 1;
    free(xml);
    return 0;
}

int fetch_weather_data(const settings_t *settings, weather_data_t *out) {
    memset(out, 0, sizeof(*out));

    double latitude = settings->latitude;
    double longitude = settings->longitude;
    if ((latitude == 0.0 && longitude == 0.0) && settings->station_grid[0]) {
        grid_to_latlon(settings->station_grid, &latitude, &longitude);
    }

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

    char current[4096] = {0};
    char daily[4096] = {0};
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

    extract_json_first_array_string(daily, "sunrise", out->sunrise, sizeof(out->sunrise));
    extract_json_first_array_string(daily, "sunset", out->sunset, sizeof(out->sunset));
    if (extract_json_first_array_number(daily, "daylight_duration", &number) == 0) out->daylight_hours = number / 3600.0;
    if (extract_json_first_array_number(daily, "temperature_2m_max", &number) == 0) out->tmax_c = number;
    if (extract_json_first_array_number(daily, "temperature_2m_min", &number) == 0) out->tmin_c = number;
    if (extract_json_first_array_number(daily, "precipitation_probability_max", &number) == 0) out->daily_precip_probability = (int)number;

    int score = 15;
    if (out->is_day) score += 5;
    if (out->cape >= 800.0) score += 20;
    else if (out->cape >= 300.0) score += 12;
    else if (out->cape >= 100.0) score += 6;
    if (out->lifted_index <= -4.0) score += 20;
    else if (out->lifted_index <= -1.0) score += 10;
    else if (out->lifted_index <= 2.0) score += 4;
    if (out->cloud_cover >= 20 && out->cloud_cover <= 80) score += 6;
    else if (out->cloud_cover < 90) score += 3;
    if (out->humidity >= 40 && out->humidity <= 90) score += 5;
    if (out->visibility_m >= 10000.0) score += 4;
    if (out->wind_kmh >= 5.0 && out->wind_kmh <= 35.0) score += 4;
    if (out->daily_precip_probability <= 30) score += 6;
    else if (out->daily_precip_probability <= 60) score += 3;
    if (out->pressure_hpa >= 1005.0 && out->pressure_hpa <= 1028.0) score += 4;

    out->sixm_weather_score = clamp_int(score, 0, 100);
    if (out->sixm_weather_score >= 70) {
        copy_string(out->sixm_weather_level, sizeof(out->sixm_weather_level), "对流活跃，可重点关注");
    } else if (out->sixm_weather_score >= 50) {
        copy_string(out->sixm_weather_level, sizeof(out->sixm_weather_level), "有一定辅助迹象");
    } else if (out->sixm_weather_score >= 35) {
        copy_string(out->sixm_weather_level, sizeof(out->sixm_weather_level), "轻度支持");
    } else {
        copy_string(out->sixm_weather_level, sizeof(out->sixm_weather_level), "参考价值有限");
    }

    out->valid = 1;
    free(json);
    return 0;
}

int fetch_tropo_data(const settings_t *settings, tropo_data_t *out) {
    memset(out, 0, sizeof(*out));
    long status = 0;
    const char *page_url = settings->tropo_source_url[0] ? settings->tropo_source_url : "https://tropo.f5len.org/asia/";
    char *html = http_get_text(page_url, NULL, &status);
    if (!html || status < 200 || status >= 300) {
        free(html);
        return -1;
    }

    tropo_image_t images[64];
    int image_count = parse_tropo_images(html, images, 64);
    free(html);
    if (image_count <= 0) {
        return -1;
    }

    int wanted = settings->tropo_forecast_hours;
    int best = 0;
    int best_diff = INT_MAX;
    for (int i = 0; i < image_count; ++i) {
        int diff = abs(images[i].hours - wanted);
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }

    size_t png_size = 0;
    char *png_data = http_get_binary(images[best].url, NULL, &status, &png_size);
    if (!png_data || status < 200 || status >= 300) {
        free(png_data);
        return -1;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *img = stbi_load_from_memory((const unsigned char *)png_data, (int)png_size, &width, &height, &channels, 3);
    free(png_data);
    if (!img) {
        return -1;
    }
    channels = 3;

    double lat = settings->latitude;
    double lon = settings->longitude;
    if ((lat == 0.0 && lon == 0.0) && settings->station_grid[0]) {
        grid_to_latlon(settings->station_grid, &lat, &lon);
    }

    const int map_x0 = 103;
    const int map_x1 = 695;
    const int map_y0 = 55;
    const int map_y1 = 563;
    const double lon_w = 111.0;
    const double lon_e = 144.0;
    const double lat_s = 20.0;
    const double lat_n = 44.0;

    double clamped_lon = clamp_double(lon, lon_w, lon_e);
    double clamped_lat = clamp_double(lat, lat_s, lat_n);
    int x = map_x0 + (int)llround((clamped_lon - lon_w) / (lon_e - lon_w) * (double)(map_x1 - map_x0));
    int y = map_y1 - (int)llround((clamped_lat - lat_s) / (lat_n - lat_s) * (double)(map_y1 - map_y0));
    x = clamp_int(x, 0, width - 1);
    y = clamp_int(y, 0, height - 1);

    int r = 0, g = 0, b = 0;
    sample_png_average(img, width, height, channels, x, y, &r, &g, &b);
    stbi_image_free(img);

    classify_tropo_pixel(r, g, b, out->category, sizeof(out->category), &out->score);
    copy_string(out->page_url, sizeof(out->page_url), page_url);
    copy_string(out->image_url, sizeof(out->image_url), images[best].url);
    out->horizon_hours = images[best].hours;
    out->sample_x = x;
    out->sample_y = y;
    out->sample_r = r;
    out->sample_g = g;
    out->sample_b = b;
    snprintf(out->summary, sizeof(out->summary),
        "按台站位置采样 F5LEN 亚洲图，对应未来 %d 小时附近的对流层条件为“%s”。该分值主要反映对流层辅助，不等价于 Sporadic-E，但在 6 米监控中可作为额外参考。",
        out->horizon_hours, out->category);
    out->valid = 1;
    return 0;
}

int fetch_meteor_data(const settings_t *settings, meteor_data_t *out) {
    memset(out, 0, sizeof(*out));
    if (!settings->meteor_enabled) {
        copy_string(out->summary, sizeof(out->summary), "流星雨提醒已关闭。");
        out->valid = 1;
        return 0;
    }

    const char *url = settings->meteor_source_url[0] ? settings->meteor_source_url : "https://www.imo.net/";
    long status = 0;
    char *html = http_get_text(url, NULL, &status);
    if (!html || status < 200 || status >= 300) {
        free(html);
        return -1;
    }

    meteor_candidate_t candidates[32];
    int candidate_count = parse_meteor_calendar_candidates(html, candidates, 32);
    if (candidate_count <= 0) {
        meteor_candidate_t item;
        if (parse_meteor_widget_candidate(html, &item) == 0) {
            candidates[0] = item;
            candidate_count = 1;
        }
    }
    free(html);

    if (candidate_count <= 0) {
        return -1;
    }

    qsort(candidates, (size_t)candidate_count, sizeof(candidates[0]), cmp_meteor_candidates);

    meteor_candidate_t selected[8];
    int selected_count = 0;
    int max_items = clamp_int(settings->meteor_max_items, 1, 8);
    for (int i = 0; i < candidate_count && selected_count < max_items; ++i) {
        if (!meteor_matches_filter(candidates[i].name, settings->meteor_selected_showers)) {
            continue;
        }
        selected[selected_count++] = candidates[i];
    }

    copy_string(out->source_url, sizeof(out->source_url), url);
    out->valid = 1;

    if (selected_count == 0) {
        if (settings->meteor_selected_showers[0]) {
            snprintf(out->summary, sizeof(out->summary),
                "流星雨日历已获取，但没有匹配到筛选项：%s。",
                settings->meteor_selected_showers);
        } else {
            copy_string(out->summary, sizeof(out->summary), "流星雨日历已获取，但没有找到可用条目。");
        }
        return 0;
    }

    copy_string(out->shower_name, sizeof(out->shower_name), selected[0].name);
    copy_string(out->peak_label, sizeof(out->peak_label), selected[0].peak_label);
    out->moon_percent = selected[0].moon_percent;
    out->days_left = selected[0].days_left;

    sb_t sb;
    sb_init(&sb);
    if (settings->meteor_selected_showers[0]) {
        sb_appendf(&sb, "已筛选流星雨 %d 项：", selected_count);
    } else {
        sb_appendf(&sb, "近期主要流星雨 %d 项：", selected_count);
    }
    for (int i = 0; i < selected_count; ++i) {
        if (i > 0) {
            sb_append(&sb, "；");
        }
        if (selected[i].days_left >= 0) {
            sb_appendf(&sb, "%s（%s，距今约 %d 天，月相约 %d%%）",
                selected[i].name, selected[i].peak_label, selected[i].days_left, selected[i].moon_percent);
        } else {
            sb_appendf(&sb, "%s（%s，月相约 %d%%）",
                selected[i].name, selected[i].peak_label, selected[i].moon_percent);
        }
    }
    copy_string(out->summary, sizeof(out->summary), sb.data ? sb.data : "");
    sb_free(&sb);
    return 0;
}

int fetch_satellite_data(const settings_t *settings, satellite_summary_t *out, app_t *app) {
    memset(out, 0, sizeof(*out));
    copy_string(out->source_url, sizeof(out->source_url),
        settings->satellite_source_url[0] ? settings->satellite_source_url : settings->satellite_api_base);

    if (!settings->satellite_enabled) {
        copy_string(out->summary, sizeof(out->summary), "卫星推荐已关闭。");
        out->valid = 1;
        return 0;
    }
    if (!settings->satellite_api_base[0] || !settings->satellite_api_key[0]) {
        copy_string(out->summary, sizeof(out->summary), "卫星推荐未启用：请在后台填写 N2YO API 地址与 API Key。");
        out->valid = 1;
        return 0;
    }

    satellite_t satellites[MAX_SATELLITES];
    int sat_count = 0;
    storage_load_satellites(app, satellites, MAX_SATELLITES, &sat_count);

    for (int i = 0; i < sat_count && out->pass_count < settings->satellite_max_items; ++i) {
        if (!satellites[i].enabled || !satellite_mode_allowed(&satellites[i], settings)) {
            continue;
        }
        char url[1024];
        snprintf(url, sizeof(url), "%sradiopasses/%d/%.6f/%.6f/%.1f/%d/%d/&apiKey=%s",
            settings->satellite_api_base,
            satellites[i].norad_id,
            settings->latitude,
            settings->longitude,
            settings->altitude_m,
            settings->satellite_days,
            settings->satellite_min_elevation,
            settings->satellite_api_key);
        long status = 0;
        char *json = http_get_text(url, NULL, &status);
        if (!json || status < 200 || status >= 300) {
            free(json);
            continue;
        }
        char passes[32768];
        if (extract_json_array(json, "passes", passes, sizeof(passes)) == 0) {
            const char *p = passes;
            while ((p = strchr(p, '{')) != NULL && out->pass_count < settings->satellite_max_items) {
                char obj[4096];
                if (copy_balanced_json(p, '{', '}', obj, sizeof(obj)) != 0) {
                    break;
                }
                double start_utc = 0.0;
                double max_utc = 0.0;
                double max_el = 0.0;
                if (extract_json_number(obj, "startUTC", &start_utc) != 0 ||
                    extract_json_number(obj, "maxUTC", &max_utc) != 0 ||
                    extract_json_number(obj, "maxEl", &max_el) != 0) {
                    p++;
                    continue;
                }
                time_t start_time = (time_t)start_utc;
                time_t max_time = (time_t)max_utc;
                if (!time_in_window_local(start_time, settings->satellite_window_start, settings->satellite_window_end)) {
                    p += strlen(obj);
                    continue;
                }
                satellite_pass_t *pass = &out->passes[out->pass_count++];
                memset(pass, 0, sizeof(*pass));
                pass->valid = 1;
                pass->norad_id = satellites[i].norad_id;
                pass->max_elevation = max_el;
                copy_string(pass->name, sizeof(pass->name), satellites[i].name);
                copy_string(pass->mode_type, sizeof(pass->mode_type), satellites[i].mode_type);
                copy_string(pass->source_url, sizeof(pass->source_url), url);
                format_time_local(start_time, pass->start_local, sizeof(pass->start_local));
                format_time_local(max_time, pass->max_local, sizeof(pass->max_local));
                p += strlen(obj);
            }
        }
        free(json);
    }

    if (out->pass_count > 1) {
        qsort(out->passes, (size_t)out->pass_count, sizeof(out->passes[0]), cmp_passes);
    }

    sb_t sb;
    sb_init(&sb);
    if (out->pass_count == 0) {
        sb_append(&sb, "今日筛选窗口内暂无符合条件的卫星过境。");
    } else {
        sb_appendf(&sb, "推荐卫星过境 %d 条：", out->pass_count);
        for (int i = 0; i < out->pass_count; ++i) {
            sb_appendf(&sb, "%s%s %s，最大仰角 %.0f°",
                i == 0 ? "\n" : "\n",
                out->passes[i].name,
                out->passes[i].start_local,
                out->passes[i].max_elevation);
        }
    }
    copy_string(out->summary, sizeof(out->summary), sb.data ? sb.data : "");
    sb_free(&sb);
    out->valid = 1;
    return 0;
}

static void build_hamqsl_section(const settings_t *settings, const hamqsl_data_t *ham, char *out, size_t out_len) {
    if (!ham->valid) {
        copy_string(out, out_len, "HAMqsl 数据暂不可用。");
        return;
    }
    sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb, "HAMqsl 更新时间：%s", ham->updated);
    if (ham_field_enabled(settings, "solarflux")) append_key_value_int(&sb, "SFI", ham->solarflux);
    if (ham_field_enabled(settings, "aindex")) append_key_value_int(&sb, "A 指数", ham->aindex);
    if (ham_field_enabled(settings, "kindex")) append_key_value_int(&sb, "K 指数", ham->kindex);
    if (ham_field_enabled(settings, "xray")) append_key_value(&sb, "X-Ray", ham->xray);
    if (ham_field_enabled(settings, "sunspots")) append_key_value_int(&sb, "黑子数", ham->sunspots);
    if (ham_field_enabled(settings, "heliumline")) append_key_value_double(&sb, "Helium line", ham->heliumline, "", 1);
    if (ham_field_enabled(settings, "protonflux")) append_key_value_int(&sb, "质子通量", ham->protonflux);
    if (ham_field_enabled(settings, "electronflux")) append_key_value_int(&sb, "电子通量", ham->electronflux);
    if (ham_field_enabled(settings, "aurora")) append_key_value_int(&sb, "极光指数", ham->aurora);
    if (ham_field_enabled(settings, "normalization")) append_key_value_double(&sb, "Normalization", ham->normalization, "", 2);
    if (ham_field_enabled(settings, "latdegree")) append_key_value_double(&sb, "极区边界", ham->latdegree, "°", 1);
    if (ham_field_enabled(settings, "solarwind")) append_key_value_double(&sb, "太阳风", ham->solarwind, " km/s", 1);
    if (ham_field_enabled(settings, "magneticfield")) append_key_value_double(&sb, "磁场强度", ham->magneticfield, " nT", 1);
    if (ham_field_enabled(settings, "geomagfield")) append_key_value(&sb, "地磁状态", ham->geomagfield);
    if (ham_field_enabled(settings, "signalnoise")) append_key_value(&sb, "噪声", ham->signalnoise);
    if (ham_field_enabled(settings, "fof2")) append_key_value(&sb, "foF2", ham->fof2[0] ? ham->fof2 : "未报");
    if (ham_field_enabled(settings, "muffactor")) append_key_value(&sb, "MUF Factor", ham->muffactor[0] ? ham->muffactor : "未报");
    if (ham_field_enabled(settings, "muf")) append_key_value(&sb, "MUF", ham->muf[0] ? ham->muf : "未报");
    append_band_summary(&sb, ham, "day", "HF 白天");
    append_band_summary(&sb, ham, "night", "HF 夜间");
    if (ham->vhf_count > 0) {
        sb_append(&sb, "\nVHF 现象：");
        for (int i = 0; i < ham->vhf_count; ++i) {
            if (i > 0) sb_append(&sb, " | ");
            sb_appendf(&sb, "%s/%s %s", ham->vhf[i].name, ham->vhf[i].location, ham->vhf[i].status);
        }
    }
    copy_string(out, out_len, sb.data ? sb.data : "");
    sb_free(&sb);
}

static void build_weather_section(const weather_data_t *weather, char *out, size_t out_len) {
    if (!weather->valid) {
        copy_string(out, out_len, "天气辅助数据暂不可用。");
        return;
    }
    snprintf(out, out_len,
        "当地天气：%.1f°C，湿度 %d%%，露点 %.1f°C，气压 %.1f hPa，云量 %d%%，能见度 %.0f m，风速 %.1f km/h。\n"
        "日照：日出 %s，日落 %s，日照 %.1f 小时，最高 %.1f°C，最低 %.1f°C。\n"
        "6 米气象辅助：%s，分值 %d/100（CAPE %.0f，Lifted Index %.1f，降水概率 %d%%）。",
        weather->temperature_c,
        weather->humidity,
        weather->dewpoint_c,
        weather->pressure_hpa,
        weather->cloud_cover,
        weather->visibility_m,
        weather->wind_kmh,
        weather->sunrise,
        weather->sunset,
        weather->daylight_hours,
        weather->tmax_c,
        weather->tmin_c,
        weather->sixm_weather_level,
        weather->sixm_weather_score,
        weather->cape,
        weather->lifted_index,
        weather->daily_precip_probability);
}

static void build_tropo_section(const settings_t *settings, const tropo_data_t *tropo, char *out, size_t out_len) {
    if (!tropo->valid) {
        copy_string(out, out_len, "F5LEN 亚洲对流层图暂不可用。");
        return;
    }
    sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb,
        "F5LEN 亚洲图 %d 小时预报：采样类别 %s，分值 %d/100，像素颜色 RGB(%d,%d,%d)。\n%s",
        tropo->horizon_hours,
        tropo->category,
        tropo->score,
        tropo->sample_r,
        tropo->sample_g,
        tropo->sample_b,
        tropo->summary);
    if (settings->tropo_send_image && tropo->image_url[0]) {
        sb_appendf(&sb, "\n热力图：[CQ:image,file=%s]", tropo->image_url);
    }
    copy_string(out, out_len, sb.data ? sb.data : "");
    sb_free(&sb);
}

static void build_solar_section(const hamqsl_data_t *ham, char *summary, size_t summary_len,
                                char *section, size_t section_len) {
    if (!ham->valid) {
        copy_string(summary, summary_len, "太阳与地磁状态暂不可用。");
        copy_string(section, section_len, "太阳与地磁状态暂不可用。");
        return;
    }
    int geomag_g = geomag_g_from_k(ham->kindex);
    snprintf(summary, summary_len,
        "太阳通量 %d，A=%d，K=%d，地磁 %s，X-Ray %s，黑子 %d，MUF %s。",
        ham->solarflux, ham->aindex, ham->kindex, ham->geomagfield,
        ham->xray, ham->sunspots, ham->muf[0] ? ham->muf : "未报");
    snprintf(section, section_len,
        "%s\n当前地磁风暴等级：G%d，太阳风 %.1f km/s，磁场 %.1f nT，极光指数 %d，噪声 %s。",
        summary, geomag_g, ham->solarwind, ham->magneticfield, ham->aurora, ham->signalnoise);
}

static void build_meteor_section(const meteor_data_t *meteor, char *out, size_t out_len) {
    if (!meteor->valid) {
        copy_string(out, out_len, "流星雨倒计时暂不可用。");
        return;
    }
    if (meteor->summary[0]) {
        copy_string(out, out_len, meteor->summary);
        return;
    }
    if (meteor->days_left >= 0) {
        snprintf(out, out_len, "最近主要流星雨：%s，峰值夜 %s，距今约 %d 天，月相约 %d%%。",
            meteor->shower_name, meteor->peak_label, meteor->days_left, meteor->moon_percent);
    } else {
        snprintf(out, out_len, "最近主要流星雨：%s，峰值夜 %s，月相约 %d%%。",
            meteor->shower_name, meteor->peak_label, meteor->moon_percent);
    }
}

static void build_satellite_section(const satellite_summary_t *satellite, char *out, size_t out_len) {
    if (!satellite->valid) {
        copy_string(out, out_len, "卫星星历暂不可用。");
        return;
    }
    copy_string(out, out_len, satellite->summary);
}

static void build_sixm_section(const settings_t *settings, const snapshot_t *snapshot, char *out, size_t out_len) {
    sb_t sb;
    sb_init(&sb);
    sb_appendf(&sb,
        "PSKReporter：15 分钟本地 %d 条，60 分钟本地 %d 条，60 分钟全球 %d 条，判断 %s，置信度 %s，分值 %d/100。",
        snapshot->psk.local_spots_15m,
        snapshot->psk.local_spots_60m,
        snapshot->psk.global_spots_60m,
        snapshot->psk.assessment,
        snapshot->psk.confidence,
        snapshot->psk.score);
    if (snapshot->psk.latest_pair[0]) {
        sb_appendf(&sb, "\n最近相关 spot：%s @ %s。", snapshot->psk.latest_pair, snapshot->psk.latest_local_time);
    }
    if (snapshot->psk.matched_grids[0]) {
        sb_appendf(&sb, "\n监控网格命中：%s。", snapshot->psk.matched_grids);
    }
    if (snapshot->psk.farthest_peer[0]) {
        sb_appendf(&sb, "\n最远相关路径：%s %s，约 %d km。", snapshot->psk.farthest_peer,
            snapshot->psk.farthest_grid, snapshot->psk.longest_path_km);
    }
    sb_appendf(&sb, "\n综合提醒级别：%s。", sixm_alert_label_from_snapshot(snapshot, settings));
    copy_string(out, out_len, sb.data ? sb.data : "");
    sb_free(&sb);
}

static void build_sources_section(const settings_t *settings, const snapshot_t *snapshot, char *out, size_t out_len) {
    sb_t sb;
    sb_init(&sb);
    if (settings->include_source_urls) {
        if (snapshot->hamqsl.valid) {
            sb_appendf(&sb, "HAMqsl：%s", snapshot->hamqsl.source_url[0] ? snapshot->hamqsl.source_url : "https://www.hamqsl.com/solarxml.php");
        }
        if (snapshot->tropo.valid) {
            sb_appendf(&sb, "%sF5LEN Tropo：%s", sb.len ? "\n" : "", snapshot->tropo.page_url[0] ? snapshot->tropo.page_url : settings->tropo_source_url);
        }
        if (snapshot->meteor.valid) {
            sb_appendf(&sb, "%s流星雨：%s", sb.len ? "\n" : "", snapshot->meteor.source_url[0] ? snapshot->meteor.source_url : settings->meteor_source_url);
        }
        if (snapshot->satellite.valid && settings->satellite_source_url[0]) {
            sb_appendf(&sb, "%s卫星星历：%s", sb.len ? "\n" : "", settings->satellite_source_url);
        }
        sb_appendf(&sb, "%s天气：Open-Meteo", sb.len ? "\n" : "");
        sb_appendf(&sb, "%sPSK 实时：mqtt.pskreporter.info", sb.len ? "\n" : "");
    }
    if (settings->include_hamqsl_widget && settings->hamqsl_widget_url[0]) {
        sb_appendf(&sb, "%sHAMqsl 日图：[CQ:image,file=%s]", sb.len ? "\n" : "", settings->hamqsl_widget_url);
    }
    if (settings->tropo_send_image && snapshot->tropo.valid && snapshot->tropo.image_url[0]) {
        sb_appendf(&sb, "%sF5LEN 图：[CQ:image,file=%s]", sb.len ? "\n" : "", snapshot->tropo.image_url);
    }
    copy_string(out, out_len, sb.data ? sb.data : "");
    sb_free(&sb);
}

static void build_analysis_summary(const settings_t *settings, const snapshot_t *snapshot, char *out, size_t out_len) {
    snprintf(out, out_len,
        "太阳面：%s\n"
        "6 米综合：PSK 判断为“%s”，F5LEN 对流层为“%s”，气象辅助为“%s”，当前建议级别为“%s”。\n"
        "运维提示：抓取频率按独立轮询处理，手动查询不会重置周期。",
        snapshot->sun_summary,
        snapshot->psk.assessment,
        snapshot->tropo.valid ? snapshot->tropo.category : "未知",
        snapshot->weather.valid ? snapshot->weather.sixm_weather_level : "未知",
        sixm_alert_label_from_snapshot(snapshot, settings));
}

void build_reports(app_t *app, snapshot_t *snapshot) {
    settings_t settings;
    memset(&settings, 0, sizeof(settings));
    if (app) {
        pthread_mutex_lock(&app->cache_mutex);
        settings = app->settings;
        pthread_mutex_unlock(&app->cache_mutex);
    }

    build_hamqsl_section(&settings, &snapshot->hamqsl, snapshot->section_hamqsl, sizeof(snapshot->section_hamqsl));
    build_weather_section(&snapshot->weather, snapshot->section_weather, sizeof(snapshot->section_weather));
    build_tropo_section(&settings, &snapshot->tropo, snapshot->section_tropo, sizeof(snapshot->section_tropo));
    build_solar_section(&snapshot->hamqsl, snapshot->sun_summary, sizeof(snapshot->sun_summary),
        snapshot->section_solar, sizeof(snapshot->section_solar));
    build_meteor_section(&snapshot->meteor, snapshot->section_meteor, sizeof(snapshot->section_meteor));
    build_satellite_section(&snapshot->satellite, snapshot->section_satellite, sizeof(snapshot->section_satellite));
    build_sixm_section(&settings, snapshot, snapshot->section_6m, sizeof(snapshot->section_6m));
    build_sources_section(&settings, snapshot, snapshot->section_sources, sizeof(snapshot->section_sources));
    build_analysis_summary(&settings, snapshot, snapshot->analysis_summary, sizeof(snapshot->analysis_summary));

    char refreshed[64];
    char ham_solarflux[32];
    char ham_aindex[32];
    char ham_kindex[32];
    char ham_sunspots[32];
    char ham_geomag_g[32];
    char weather_score[32];
    char tropo_score[32];
    char meteor_days_left[32];
    format_time_local(snapshot->refreshed_at, refreshed, sizeof(refreshed));
    snprintf(ham_solarflux, sizeof(ham_solarflux), "%d", snapshot->hamqsl.solarflux);
    snprintf(ham_aindex, sizeof(ham_aindex), "%d", snapshot->hamqsl.aindex);
    snprintf(ham_kindex, sizeof(ham_kindex), "%d", snapshot->hamqsl.kindex);
    snprintf(ham_sunspots, sizeof(ham_sunspots), "%d", snapshot->hamqsl.sunspots);
    snprintf(ham_geomag_g, sizeof(ham_geomag_g), "%d", geomag_g_from_k(snapshot->hamqsl.kindex));
    snprintf(weather_score, sizeof(weather_score), "%d", snapshot->weather.sixm_weather_score);
    snprintf(tropo_score, sizeof(tropo_score), "%d", snapshot->tropo.score);
    snprintf(meteor_days_left, sizeof(meteor_days_left), "%d", snapshot->meteor.days_left);

    const char *sixm_label = sixm_alert_label_from_snapshot(snapshot, &settings);
    template_token_t tokens[] = {
        {"bot_name", settings.bot_name[0] ? settings.bot_name : APP_NAME},
        {"station_name", settings.station_name},
        {"station_grid", settings.station_grid},
        {"psk_grids", settings.psk_grids},
        {"refreshed_at", refreshed},
        {"section_hamqsl", snapshot->section_hamqsl},
        {"section_weather", snapshot->section_weather},
        {"section_tropo", snapshot->section_tropo},
        {"section_6m", snapshot->section_6m},
        {"section_solar", snapshot->section_solar},
        {"section_meteor", snapshot->section_meteor},
        {"section_satellite", snapshot->section_satellite},
        {"section_sources", snapshot->section_sources},
        {"analysis_summary", snapshot->analysis_summary},
        {"ham_solarflux", ham_solarflux},
        {"ham_aindex", ham_aindex},
        {"ham_kindex", ham_kindex},
        {"ham_xray", snapshot->hamqsl.xray},
        {"ham_sunspots", ham_sunspots},
        {"ham_geomagfield", snapshot->hamqsl.geomagfield},
        {"ham_muf", snapshot->hamqsl.muf},
        {"weather_level", snapshot->weather.sixm_weather_level},
        {"weather_score", weather_score},
        {"tropo_category", snapshot->tropo.category},
        {"tropo_score", tropo_score},
        {"meteor_name", snapshot->meteor.shower_name},
        {"meteor_peak", snapshot->meteor.peak_label},
        {"meteor_days_left", meteor_days_left},
        {"geomag_g", ham_geomag_g},
        {"sixm_alert_level", sixm_label}
    };
    size_t token_count = sizeof(tokens) / sizeof(tokens[0]);

    render_template(snapshot->report_text, sizeof(snapshot->report_text), settings.report_template_full, tokens, token_count);
    render_template(snapshot->report_6m, sizeof(snapshot->report_6m), settings.report_template_6m, tokens, token_count);
    render_template(snapshot->report_solar, sizeof(snapshot->report_solar), settings.report_template_solar, tokens, token_count);
    render_template(snapshot->report_geomag, sizeof(snapshot->report_geomag), settings.report_template_geomag, tokens, token_count);
    render_template(snapshot->report_open6m, sizeof(snapshot->report_open6m), settings.report_template_open6m, tokens, token_count);
    render_template(snapshot->report_help, sizeof(snapshot->report_help), settings.help_template, tokens, token_count);
}
