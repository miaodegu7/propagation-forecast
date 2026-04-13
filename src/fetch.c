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

static int render_template(char *out, size_t out_len, const char *tmpl,
                           const template_token_t *tokens, size_t token_count);

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

static void append_csv_value(char *csv, size_t csv_len, const char *value) {
    if (!csv || csv_len == 0 || !value || !*value) {
        return;
    }
    if (csv_contains_ci(csv, value)) {
        return;
    }
    if (csv[0]) {
        strncat(csv, ", ", csv_len - strlen(csv) - 1);
    }
    strncat(csv, value, csv_len - strlen(csv) - 1);
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

static const char *meteor_name_to_chinese(const char *name) {
    static const struct {
        const char *en;
        const char *cn;
    } aliases[] = {
        {"Quadrantids", "象限仪座流星雨"},
        {"Lyrids", "天琴座流星雨"},
        {"Eta Aquariids", "宝瓶座η流星雨"},
        {"June Bootids", "六月牧夫座流星雨"},
        {"Alpha Capricornids", "摩羯座α流星雨"},
        {"Southern Delta Aquariids", "南宝瓶座δ流星雨"},
        {"Perseids", "英仙座流星雨"},
        {"Draconids", "天龙座流星雨"},
        {"Orionids", "猎户座流星雨"},
        {"Southern Taurids", "南金牛座流星雨"},
        {"Northern Taurids", "北金牛座流星雨"},
        {"Leonids", "狮子座流星雨"},
        {"Geminids", "双子座流星雨"},
        {"Ursids", "小熊座流星雨"},
        {"Arietids", "白羊座流星雨"}
    };
    if (!name || !*name) {
        return "";
    }
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i) {
        if (strcasecmp(name, aliases[i].en) == 0 || string_contains_ci(name, aliases[i].en)) {
            return aliases[i].cn;
        }
    }
    struct {
        const char *en;
        const char *cn;
    } items[] = {
        {"Quadrantids", "象限仪座流星雨"},
        {"Lyrids", "天琴座流星雨"},
        {"Eta Aquariids", "宝瓶座η流星雨"},
        {"June Bootids", "六月牧夫座流星雨"},
        {"Alpha Capricornids", "摩羯座α流星雨"},
        {"Southern Delta Aquariids", "南宝瓶座δ流星雨"},
        {"Perseids", "英仙座流星雨"},
        {"Draconids", "天龙座流星雨"},
        {"Orionids", "猎户座流星雨"},
        {"Southern Taurids", "南金牛座流星雨"},
        {"Northern Taurids", "北金牛座流星雨"},
        {"Leonids", "狮子座流星雨"},
        {"Geminids", "双子座流星雨"},
        {"Ursids", "小熊座流星雨"},
        {"Arietids", "白羊座流星雨"}
    };
    if (!name || !*name) {
        return "";
    }
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        if (strcasecmp(name, items[i].en) == 0) {
            return items[i].cn;
        }
    }
    return name;
}

static void format_peak_date_cn(const char *peak_label, char *out, size_t out_len) {
    struct tm peak_tm;
    if (!peak_label || !*peak_label) {
        copy_string(out, out_len, "");
        return;
    }
    if (parse_calendar_peak_date(peak_label, &peak_tm) != 0 &&
        parse_peak_date(peak_label, &peak_tm) != 0) {
        copy_string(out, out_len, peak_label);
        return;
    }
    snprintf(out, out_len, "%04d年%02d月%02d日",
        peak_tm.tm_year + 1900,
        peak_tm.tm_mon + 1,
        peak_tm.tm_mday);
}

static void format_peak_date_cn_text(const char *peak_label, char *out, size_t out_len) {
    struct tm peak_tm;
    if (!peak_label || !*peak_label) {
        copy_string(out, out_len, "");
        return;
    }
    if (parse_calendar_peak_date(peak_label, &peak_tm) != 0 &&
        parse_peak_date(peak_label, &peak_tm) != 0) {
        copy_string(out, out_len, peak_label);
        return;
    }
    snprintf(out, out_len, "%04d年%02d月%02d日",
        peak_tm.tm_year + 1900,
        peak_tm.tm_mon + 1,
        peak_tm.tm_mday);
}

static void format_meteor_countdown_text(int days_left, char *out, size_t out_len) {
    if (days_left > 0) {
        snprintf(out, out_len, "还有 %d 天", days_left);
    } else if (days_left == 0) {
        copy_string(out, out_len, "就是今天");
    } else {
        copy_string(out, out_len, "已过");
    }
}

static void build_band_summary_text(const hamqsl_data_t *ham, const char *time_slot, char *out, size_t out_len) {
    sb_t sb;
    sb_init(&sb);
    for (int i = 0; i < ham->band_count; ++i) {
        if (strcasecmp(ham->bands[i].time_slot, time_slot) != 0) {
            continue;
        }
        if (sb.len > 0) {
            sb_append(&sb, " | ");
        }
        sb_appendf(&sb, "%s %s", ham->bands[i].name, ham->bands[i].status);
    }
    copy_string(out, out_len, (sb.data && sb.data[0]) ? sb.data : "未报");
    sb_free(&sb);
}

static int meteor_matches_filter(const char *name, const char *filter_csv) {
    if (!filter_csv || !*filter_csv) {
        return 1;
    }
    char temp[MAX_HUGE_TEXT];
    const char *name_cn = meteor_name_to_chinese(name);
    copy_string(temp, sizeof(temp), filter_csv);
    char *save = NULL;
    for (char *part = strtok_r(temp, ",|/\t\r\n", &save); part; part = strtok_r(NULL, ",|/\t\r\n", &save)) {
        trim_whitespace(part);
        if (*part && (string_contains_ci(name, part) || string_contains_ci(name_cn, part))) {
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

static const char *text_or_default(const char *value, const char *fallback) {
    return (value && *value) ? value : fallback;
}

static int sixm_alert_level_from_snapshot_internal(const snapshot_t *snapshot, const settings_t *settings) {
    int psk_hot = snapshot->psk.local_spots_15m >= settings->sixm_psk_trigger_spots ||
        snapshot->psk.local_spots_60m >= settings->sixm_psk_trigger_spots;
    int psk_some = snapshot->psk.local_spots_60m > 0;
    int hamalert_hot = snapshot->psk.hamalert_hits_15m >= settings->sixm_psk_trigger_spots ||
        snapshot->psk.hamalert_hits_60m >= settings->sixm_psk_trigger_spots;
    int hamalert_some = snapshot->psk.hamalert_hits_60m > 0;
    int tropo_hot = snapshot->tropo.valid && snapshot->tropo.score >= 78;
    int tropo_some = snapshot->tropo.valid && snapshot->tropo.score >= 55;
    int weather_hot = snapshot->weather.valid && snapshot->weather.sixm_weather_score >= 70;
    int weather_some = snapshot->weather.valid && snapshot->weather.sixm_weather_score >= 50;

    if ((psk_hot || hamalert_hot) && (tropo_some || weather_some || psk_some)) return 3;
    if (psk_hot || hamalert_hot || ((psk_some || hamalert_some) && (tropo_some || weather_some)) || (tropo_hot && weather_hot)) return 2;
    if (psk_some || hamalert_some || tropo_some || weather_some) return 1;
    return 0;
}

static int twom_alert_level_from_snapshot_internal(const snapshot_t *snapshot, const settings_t *settings) {
    int psk_hot = snapshot->twom.local_spots_15m >= settings->twom_psk_trigger_spots ||
        snapshot->twom.local_spots_60m >= settings->twom_psk_trigger_spots;
    int psk_some = snapshot->twom.local_spots_60m > 0;
    int hamalert_hot = snapshot->twom.hamalert_hits_15m >= settings->twom_psk_trigger_spots ||
        snapshot->twom.hamalert_hits_60m >= settings->twom_psk_trigger_spots;
    int hamalert_some = snapshot->twom.hamalert_hits_60m > 0;
    int tropo_hot = snapshot->tropo.valid && snapshot->tropo.score >= 68;
    int tropo_some = snapshot->tropo.valid && snapshot->tropo.score >= 52;
    int weather_hot = snapshot->weather.valid && snapshot->weather.twom_weather_score >= 70;
    int weather_some = snapshot->weather.valid && snapshot->weather.twom_weather_score >= 50;

    if ((psk_hot || hamalert_hot) && (tropo_some || weather_some || psk_some)) return 3;
    if (psk_hot || hamalert_hot || (tropo_hot && weather_hot) || ((psk_some || hamalert_some) && (tropo_some || weather_some))) return 2;
    if (psk_some || hamalert_some || tropo_some || weather_some) return 1;
    return 0;
}

const char *sixm_alert_label_from_snapshot(const snapshot_t *snapshot, const settings_t *settings) {
    switch (sixm_alert_level_from_snapshot_internal(snapshot, settings)) {
        case 3: return text_or_default(settings->wording_alert_strong, "强提醒");
        case 2: return text_or_default(settings->wording_alert_watch, "重点观察");
        case 1: return text_or_default(settings->wording_alert_info, "一般提醒");
        default: return text_or_default(settings->wording_alert_none, "暂无提醒");
    }
}

const char *twom_alert_label_from_snapshot(const snapshot_t *snapshot, const settings_t *settings) {
    switch (twom_alert_level_from_snapshot_internal(snapshot, settings)) {
        case 3: return text_or_default(settings->wording_alert_strong, "强提醒");
        case 2: return text_or_default(settings->wording_alert_watch, "重点观察");
        case 1: return text_or_default(settings->wording_alert_info, "一般提醒");
        default: return text_or_default(settings->wording_alert_none, "暂无提醒");
    }
}

static const char *template_or_default(const char *tmpl, const char *fallback) {
    return (tmpl && *tmpl) ? tmpl : fallback;
}

const char *psk_assessment_code_from_summary(const psk_summary_t *summary) {
    if (!summary) return "unknown";
    if (summary->local_spots_60m == 0 && summary->hamalert_hits_15m >= 2) return "hamalert_open";
    if (summary->local_spots_60m == 0 && summary->hamalert_hits_60m > 0) return "hamalert_hint";
    if (summary->local_spots_15m >= 3 || summary->local_spots_60m >= 6) return "open";
    if (summary->local_spots_60m >= 2) return "possible";
    if (summary->global_spots_60m >= 20) return "global_only";
    if (summary->mqtt_connected) return "quiet";
    return "disconnected";
}

const char *psk_confidence_code_from_summary(const psk_summary_t *summary) {
    if (!summary) return "unknown";
    if (!summary->mqtt_connected && summary->local_spots_60m == 0 && summary->hamalert_hits_60m == 0) return "unknown";
    if (summary->local_spots_15m >= 3 || summary->local_spots_60m >= 6) return "high";
    if (summary->local_spots_60m >= 2 || summary->hamalert_hits_15m >= 2) return "medium";
    return "low";
}

const char *sixm_alert_code_from_snapshot(const snapshot_t *snapshot, const settings_t *settings) {
    switch (sixm_alert_level_from_snapshot_internal(snapshot, settings)) {
        case 3: return "strong";
        case 2: return "watch";
        case 1: return "info";
        default: return "none";
    }
}

const char *twom_alert_code_from_snapshot(const snapshot_t *snapshot, const settings_t *settings) {
    switch (twom_alert_level_from_snapshot_internal(snapshot, settings)) {
        case 3: return "strong";
        case 2: return "watch";
        case 1: return "info";
        default: return "none";
    }
}

static const char *tropo_category_code_from_label(const char *label) {
    if (!label || !*label) return "unknown";
    if (string_contains_ci(label, "波导")) return "ducting";
    if (string_contains_ci(label, "很强")) return "very_strong";
    if (string_contains_ci(label, "较强")) return "strong";
    if (string_contains_ci(label, "良好")) return "good";
    if (string_contains_ci(label, "增强")) return "enhanced";
    if (string_contains_ci(label, "受扰")) return "disturbed";
    if (string_contains_ci(label, "普通")) return "normal";
    return "custom";
}

static const char *meteor_countdown_code_from_days(int days_left) {
    if (days_left > 0) return "future";
    if (days_left == 0) return "today";
    return "past";
}

static const char *satellite_api_status_code_from_label(const char *label) {
    if (!label || !*label) return "unknown";
    if (string_contains_ci(label, "正常")) return "ok";
    if (string_contains_ci(label, "部分")) return "partial";
    if (string_contains_ci(label, "异常")) return "error";
    return "custom";
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

    score = 18;
    if (!out->is_day) score += 6;
    if (out->pressure_hpa >= 1016.0) score += 16;
    else if (out->pressure_hpa >= 1010.0) score += 10;
    else if (out->pressure_hpa >= 1005.0) score += 5;
    if (out->humidity >= 55 && out->humidity <= 95) score += 12;
    else if (out->humidity >= 40) score += 6;
    if (out->cloud_cover >= 15 && out->cloud_cover <= 75) score += 10;
    else if (out->cloud_cover < 90) score += 5;
    if (out->visibility_m >= 15000.0) score += 10;
    else if (out->visibility_m >= 8000.0) score += 6;
    if (out->wind_kmh >= 3.0 && out->wind_kmh <= 25.0) score += 8;
    else if (out->wind_kmh <= 35.0) score += 4;
    if (out->daily_precip_probability <= 25) score += 8;
    else if (out->daily_precip_probability <= 50) score += 4;
    if (out->lifted_index >= 0.0) score += 6;
    else if (out->lifted_index >= -2.0) score += 3;
    if (out->cape <= 150.0) score += 6;
    else if (out->cape <= 400.0) score += 3;
    if (out->dewpoint_c >= 0.0) score += 4;

    out->twom_weather_score = clamp_int(score, 0, 100);
    if (out->twom_weather_score >= 72) {
        copy_string(out->twom_weather_level, sizeof(out->twom_weather_level), "对流层条件较好");
    } else if (out->twom_weather_score >= 55) {
        copy_string(out->twom_weather_level, sizeof(out->twom_weather_level), "有一定对流层支持");
    } else if (out->twom_weather_score >= 40) {
        copy_string(out->twom_weather_level, sizeof(out->twom_weather_level), "可作辅助参考");
    } else {
        copy_string(out->twom_weather_level, sizeof(out->twom_weather_level), "对流层支持有限");
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

    copy_string(out->shower_name, sizeof(out->shower_name), meteor_name_to_chinese(selected[0].name));
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
                meteor_name_to_chinese(selected[i].name), selected[i].peak_label, selected[i].days_left, selected[i].moon_percent);
        } else {
            sb_appendf(&sb, "%s（%s，月相约 %d%%）",
                meteor_name_to_chinese(selected[i].name), selected[i].peak_label, selected[i].moon_percent);
        }
    }
    copy_string(out->summary, sizeof(out->summary), sb.data ? sb.data : "");
    sb_free(&sb);
    return 0;
}

int fetch_satellite_data(const settings_t *settings, satellite_summary_t *out, app_t *app) {
    memset(out, 0, sizeof(*out));
    out->enabled = settings->satellite_enabled;
    out->api_configured = settings->satellite_api_base[0] && settings->satellite_api_key[0];
    copy_string(out->source_url, sizeof(out->source_url),
        settings->satellite_source_url[0] ? settings->satellite_source_url : settings->satellite_api_base);

    satellite_t satellites[MAX_SATELLITES];
    int sat_count = 0;
    storage_load_satellites(app, satellites, MAX_SATELLITES, &sat_count);
    out->total_satellites = sat_count;
    for (int i = 0; i < sat_count; ++i) {
        if (satellites[i].enabled) {
            out->enabled_satellites++;
        }
        if (satellites[i].enabled && satellite_mode_allowed(&satellites[i], settings)) {
            out->selected_satellites++;
            append_csv_value(out->selected_names, sizeof(out->selected_names), satellites[i].name);
        }
    }

    if (!settings->satellite_enabled) {
        copy_string(out->api_status, sizeof(out->api_status), "已关闭");
        snprintf(out->summary, sizeof(out->summary),
            "卫星推荐已关闭。当前共配置 %d 颗卫星，启用 %d 颗。",
            out->total_satellites, out->enabled_satellites);
        out->valid = 1;
        return 0;
    }
    if (out->selected_satellites == 0) {
        copy_string(out->api_status, sizeof(out->api_status), "未选择");
        copy_string(out->summary, sizeof(out->summary), "当前没有启用且符合筛选条件的卫星，请先在后台勾选卫星。");
        out->valid = 1;
        return 0;
    }
    if (!out->api_configured) {
        copy_string(out->api_status, sizeof(out->api_status), "未配置");
        snprintf(out->summary, sizeof(out->summary),
            "已选择 %d 颗卫星，但未填写 N2YO API 地址或 API Key。",
            out->selected_satellites);
        out->valid = 1;
        return 0;
    }

    double latitude = settings->latitude;
    double longitude = settings->longitude;
    if ((latitude == 0.0 && longitude == 0.0) && settings->station_grid[0]) {
        grid_to_latlon(settings->station_grid, &latitude, &longitude);
    }

    for (int i = 0; i < sat_count && out->pass_count < settings->satellite_max_items; ++i) {
        if (!satellites[i].enabled || !satellite_mode_allowed(&satellites[i], settings)) {
            continue;
        }
        out->api_requests++;
        char url[1024];
        snprintf(url, sizeof(url), "%sradiopasses/%d/%.6f/%.6f/%.1f/%d/%d/&apiKey=%s",
            settings->satellite_api_base,
            satellites[i].norad_id,
            latitude,
            longitude,
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
        out->api_successes++;
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
    if (out->api_successes == 0 && out->api_requests > 0) {
        copy_string(out->api_status, sizeof(out->api_status), "接口异常");
        sb_appendf(&sb, "已选择 %d 颗卫星，但卫星 API 暂未返回可用数据。",
            out->selected_satellites);
    } else if (out->api_successes < out->api_requests) {
        copy_string(out->api_status, sizeof(out->api_status), "部分成功");
        sb_appendf(&sb, "已选择 %d 颗卫星，API 成功 %d/%d 次。",
            out->selected_satellites, out->api_successes, out->api_requests);
    } else {
        copy_string(out->api_status, sizeof(out->api_status), "正常");
        sb_appendf(&sb, "已选择 %d 颗卫星，API 成功 %d/%d 次。",
            out->selected_satellites, out->api_successes, out->api_requests);
    }

    if (out->pass_count == 0) {
        sb_append(&sb, "\n今日筛选窗口内暂无符合条件的卫星过境。");
    } else {
        sb_appendf(&sb, "\n今日筛选后命中 %d 条过境：", out->pass_count);
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
    char kindex_text[32];
    char hf_day[512];
    char hf_night[512];
    const char *tmpl = settings->compact_template_hamqsl[0] ? settings->compact_template_hamqsl :
        "更新时间：{{updated}}\nK 指数：{{kindex}}（地磁：{{geomagfield}}）\nHF 白天：{{hf_day}}\nHF 夜间：{{hf_night}}";
    if (!ham->valid) {
        copy_string(out, out_len, "HAMqsl 数据暂不可用。");
        return;
    }
    snprintf(kindex_text, sizeof(kindex_text), "%d", ham->kindex);
    build_band_summary_text(ham, "day", hf_day, sizeof(hf_day));
    build_band_summary_text(ham, "night", hf_night, sizeof(hf_night));

    template_token_t tokens[] = {
        {"updated", ham->updated},
        {"kindex", kindex_text},
        {"geomagfield", ham->geomagfield[0] ? ham->geomagfield : "未报"},
        {"hf_day", hf_day},
        {"hf_night", hf_night}
    };
    render_template(out, out_len, tmpl, tokens, sizeof(tokens) / sizeof(tokens[0]));
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

static void build_meteor_section(const settings_t *settings, const meteor_data_t *meteor, char *out, size_t out_len) {
    char meteor_name_cn[MAX_TEXT];
    char peak_date_cn[MAX_TEXT];
    char days_left_text[32];
    char countdown_text[MAX_TEXT];
    char moon_percent_text[32];
    const char *tmpl = settings->compact_template_meteor[0] ? settings->compact_template_meteor :
        "流星雨倒计时：{{meteor_name_cn}}\n峰值日期：{{peak_date_cn}}\n倒计时：{{countdown_text}}";
    if (!meteor->valid) {
        copy_string(out, out_len, "流星雨倒计时暂不可用。");
        return;
    }
    if (!meteor->shower_name[0] || !meteor->peak_label[0]) {
        copy_string(out, out_len, meteor->summary[0] ? meteor->summary : "流星雨倒计时暂不可用。");
        return;
    }

    copy_string(meteor_name_cn, sizeof(meteor_name_cn), meteor_name_to_chinese(meteor->shower_name));
    format_peak_date_cn_text(meteor->peak_label, peak_date_cn, sizeof(peak_date_cn));
    snprintf(days_left_text, sizeof(days_left_text), "%d", meteor->days_left);
    format_meteor_countdown_text(meteor->days_left, countdown_text, sizeof(countdown_text));
    snprintf(moon_percent_text, sizeof(moon_percent_text), "%d", meteor->moon_percent);

    template_token_t tokens[] = {
        {"meteor_name", meteor->shower_name},
        {"meteor_name_cn", meteor_name_cn},
        {"peak_date", meteor->peak_label},
        {"peak_date_cn", peak_date_cn},
        {"days_left", days_left_text},
        {"countdown_text", countdown_text},
        {"moon_percent", moon_percent_text}
    };
    render_template(out, out_len, tmpl, tokens, sizeof(tokens) / sizeof(tokens[0]));
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
    template_token_t tokens[] = {
        {"hamqsl_widget_url", settings->hamqsl_widget_url}
    };
    if (settings->include_hamqsl_widget && settings->hamqsl_widget_url[0]) {
        render_template(out, out_len,
            settings->compact_template_hamqsl_image[0] ? settings->compact_template_hamqsl_image :
            "HAMqsl 日图：[CQ:image,file={{hamqsl_widget_url}}]",
            tokens, sizeof(tokens) / sizeof(tokens[0]));
    } else if (settings->include_source_urls) {
        snprintf(out, out_len, "HAMqsl：%s",
            snapshot->hamqsl.source_url[0] ? snapshot->hamqsl.source_url : "https://www.hamqsl.com/solarxml.php");
    } else {
        out[0] = '\0';
    }
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

static int render_template(char *out, size_t out_len, const char *tmpl,
                           const template_token_t *tokens, size_t token_count) {
    return app_render_template(out, out_len, tmpl, tokens, token_count);
}

int sixm_alert_num_from_snapshot(const snapshot_t *snapshot, const settings_t *settings) {
    return sixm_alert_level_from_snapshot_internal(snapshot, settings);
}

int twom_alert_num_from_snapshot(const snapshot_t *snapshot, const settings_t *settings) {
    return twom_alert_level_from_snapshot_internal(snapshot, settings);
}

static void inline_copy(char *out, size_t out_len, const char *text) {
    sb_t sb;
    const char *src = text ? text : "";
    sb_init(&sb);
    while (*src) {
        if (*src == '\r' || *src == '\n' || *src == '\t') {
            if (sb.len == 0 || sb.data[sb.len - 1] != ' ') {
                sb_append(&sb, " ");
            }
        } else {
            char ch[2] = {*src, '\0'};
            sb_append(&sb, ch);
        }
        src++;
    }
    if (sb.data) {
        trim_whitespace(sb.data);
    }
    copy_string(out, out_len, sb.data ? sb.data : "");
    sb_free(&sb);
}

static void build_hamqsl_section_custom(const settings_t *settings, const hamqsl_data_t *ham, char *out, size_t out_len) {
    char kindex_text[32] = "";
    char solarflux_text[32] = "";
    char aindex_text[32] = "";
    char sunspots_text[32] = "";
    char aurora_text[32] = "";
    char solarwind_text[32] = "";
    char magneticfield_text[32] = "";
    char hf_day[512] = "";
    char hf_night[512] = "";
    template_token_t tokens[] = {
        {"updated", ham->updated},
        {"kindex", kindex_text},
        {"ham_kindex", kindex_text},
        {"ham_solarflux", solarflux_text},
        {"ham_aindex", aindex_text},
        {"ham_xray", ham->xray},
        {"ham_sunspots", sunspots_text},
        {"ham_muf", ham->muf},
        {"ham_geomagfield", ham->geomagfield},
        {"geomagfield", ham->geomagfield},
        {"ham_signalnoise", ham->signalnoise},
        {"ham_fof2", ham->fof2},
        {"ham_muffactor", ham->muffactor},
        {"ham_kindex_text", ham->kindex_text},
        {"ham_source_name", ham->source_name},
        {"ham_source_url", ham->source_url},
        {"hf_day", hf_day},
        {"hf_night", hf_night},
        {"ham_aurora", aurora_text},
        {"ham_solarwind", solarwind_text},
        {"ham_magneticfield", magneticfield_text}
    };

    if (!ham->valid) {
        app_render_template(out, out_len,
            template_or_default(settings->compact_template_hamqsl_unavailable, "HAMqsl 数据暂不可用。"),
            tokens, sizeof(tokens) / sizeof(tokens[0]));
        return;
    }

    snprintf(kindex_text, sizeof(kindex_text), "%d", ham->kindex);
    snprintf(solarflux_text, sizeof(solarflux_text), "%d", ham->solarflux);
    snprintf(aindex_text, sizeof(aindex_text), "%d", ham->aindex);
    snprintf(sunspots_text, sizeof(sunspots_text), "%d", ham->sunspots);
    snprintf(aurora_text, sizeof(aurora_text), "%d", ham->aurora);
    snprintf(solarwind_text, sizeof(solarwind_text), "%.1f", ham->solarwind);
    snprintf(magneticfield_text, sizeof(magneticfield_text), "%.1f", ham->magneticfield);
    build_band_summary_text(ham, "day", hf_day, sizeof(hf_day));
    build_band_summary_text(ham, "night", hf_night, sizeof(hf_night));

    app_render_template(out, out_len,
        template_or_default(settings->compact_template_hamqsl,
            "更新时间：{{updated}}\nK 指数：{{kindex}}（地磁：{{geomagfield}}）\nHF 白天：{{hf_day}}\nHF 夜间：{{hf_night}}"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_weather_section_custom(const settings_t *settings, const weather_data_t *weather, char *out, size_t out_len) {
    char is_day_text[8] = "";
    char temperature_text[32] = "";
    char humidity_text[32] = "";
    char dewpoint_text[32] = "";
    char pressure_text[32] = "";
    char cloud_text[32] = "";
    char visibility_text[32] = "";
    char wind_text[32] = "";
    char cape_text[32] = "";
    char lifted_text[32] = "";
    char precip_text[32] = "";
    char daylight_text[32] = "";
    char tmax_text[32] = "";
    char tmin_text[32] = "";
    char daily_precip_text[32] = "";
    char weather_score_text[32] = "";
    char twom_weather_score_text[32] = "";
    template_token_t tokens[] = {
        {"weather_current_time", weather->current_time},
        {"weather_is_day", is_day_text},
        {"temperature_c", temperature_text},
        {"humidity", humidity_text},
        {"dewpoint_c", dewpoint_text},
        {"pressure_hpa", pressure_text},
        {"cloud_cover", cloud_text},
        {"visibility_m", visibility_text},
        {"wind_kmh", wind_text},
        {"cape", cape_text},
        {"lifted_index", lifted_text},
        {"precipitation_probability", precip_text},
        {"sunrise", weather->sunrise},
        {"sunset", weather->sunset},
        {"daylight_hours", daylight_text},
        {"tmax_c", tmax_text},
        {"tmin_c", tmin_text},
        {"daily_precip_probability", daily_precip_text},
        {"weather_level", weather->sixm_weather_level},
        {"weather_score", weather_score_text}
        ,
        {"twom_weather_level", weather->twom_weather_level},
        {"twom_weather_score", twom_weather_score_text}
    };

    if (!weather->valid) {
        app_render_template(out, out_len,
            template_or_default(settings->section_template_weather_unavailable, "天气辅助数据暂不可用。"),
            tokens, sizeof(tokens) / sizeof(tokens[0]));
        return;
    }

    snprintf(is_day_text, sizeof(is_day_text), "%d", weather->is_day ? 1 : 0);
    snprintf(temperature_text, sizeof(temperature_text), "%.1f", weather->temperature_c);
    snprintf(humidity_text, sizeof(humidity_text), "%d", weather->humidity);
    snprintf(dewpoint_text, sizeof(dewpoint_text), "%.1f", weather->dewpoint_c);
    snprintf(pressure_text, sizeof(pressure_text), "%.1f", weather->pressure_hpa);
    snprintf(cloud_text, sizeof(cloud_text), "%d", weather->cloud_cover);
    snprintf(visibility_text, sizeof(visibility_text), "%.0f", weather->visibility_m);
    snprintf(wind_text, sizeof(wind_text), "%.1f", weather->wind_kmh);
    snprintf(cape_text, sizeof(cape_text), "%.0f", weather->cape);
    snprintf(lifted_text, sizeof(lifted_text), "%.1f", weather->lifted_index);
    snprintf(precip_text, sizeof(precip_text), "%d", weather->precipitation_probability);
    snprintf(daylight_text, sizeof(daylight_text), "%.1f", weather->daylight_hours);
    snprintf(tmax_text, sizeof(tmax_text), "%.1f", weather->tmax_c);
    snprintf(tmin_text, sizeof(tmin_text), "%.1f", weather->tmin_c);
    snprintf(daily_precip_text, sizeof(daily_precip_text), "%d", weather->daily_precip_probability);
    snprintf(weather_score_text, sizeof(weather_score_text), "%d", weather->sixm_weather_score);
    snprintf(twom_weather_score_text, sizeof(twom_weather_score_text), "%d", weather->twom_weather_score);

    app_render_template(out, out_len,
        template_or_default(settings->section_template_weather,
            "当地天气：{{temperature_c}}°C，湿度 {{humidity}}%，露点 {{dewpoint_c}}°C，气压 {{pressure_hpa}} hPa，云量 {{cloud_cover}}%，能见度 {{visibility_m}} m，风速 {{wind_kmh}} km/h。\n日照：日出 {{sunrise}}，日落 {{sunset}}，日照 {{daylight_hours}} 小时，最高 {{tmax_c}}°C，最低 {{tmin_c}}°C。\n6m 气象辅助：{{weather_level}}，分值 {{weather_score}}/100（CAPE {{cape}}，Lifted Index {{lifted_index}}，降水概率 {{daily_precip_probability}}%）。"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_tropo_section_custom(const settings_t *settings, const tropo_data_t *tropo, char *out, size_t out_len) {
    char horizon_text[32] = "";
    char score_text[32] = "";
    char sample_r_text[32] = "";
    char sample_g_text[32] = "";
    char sample_b_text[32] = "";
    char image_cq[MAX_LARGE_TEXT + 32] = "";
    char category_code[MAX_TEXT] = "";
    template_token_t tokens[] = {
        {"tropo_hours", horizon_text},
        {"tropo_category", tropo->category},
        {"tropo_category_code", category_code},
        {"tropo_score", score_text},
        {"tropo_sample_r", sample_r_text},
        {"tropo_sample_g", sample_g_text},
        {"tropo_sample_b", sample_b_text},
        {"tropo_summary", tropo->summary},
        {"tropo_page_url", tropo->page_url},
        {"tropo_image_url", tropo->image_url},
        {"tropo_image_cq", image_cq}
    };

    if (!tropo->valid) {
        app_render_template(out, out_len,
            template_or_default(settings->section_template_tropo_unavailable, "F5LEN 传播图暂不可用。"),
            tokens, sizeof(tokens) / sizeof(tokens[0]));
        return;
    }

    snprintf(horizon_text, sizeof(horizon_text), "%d", tropo->horizon_hours);
    snprintf(score_text, sizeof(score_text), "%d", tropo->score);
    snprintf(sample_r_text, sizeof(sample_r_text), "%d", tropo->sample_r);
    snprintf(sample_g_text, sizeof(sample_g_text), "%d", tropo->sample_g);
    snprintf(sample_b_text, sizeof(sample_b_text), "%d", tropo->sample_b);
    copy_string(category_code, sizeof(category_code), tropo_category_code_from_label(tropo->category));
    if (settings->tropo_send_image && tropo->image_url[0]) {
        snprintf(image_cq, sizeof(image_cq), "[CQ:image,file=%s]", tropo->image_url);
    }

    app_render_template(out, out_len,
        template_or_default(settings->section_template_tropo,
            "F5LEN 亚洲图 {{tropo_hours}} 小时预报：类别 {{tropo_category}}，分值 {{tropo_score}}/100，像素 RGB({{tropo_sample_r}},{{tropo_sample_g}},{{tropo_sample_b}})。\n{{tropo_summary}}\n{{tropo_image_cq}}"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_solar_section_custom(const settings_t *settings, const hamqsl_data_t *ham,
                                       char *summary, size_t summary_len,
                                       char *section, size_t section_len) {
    char geomag_g_text[32] = "";
    char solarflux_text[32] = "";
    char aindex_text[32] = "";
    char kindex_text[32] = "";
    char sunspots_text[32] = "";
    char aurora_text[32] = "";
    char solarwind_text[32] = "";
    char magneticfield_text[32] = "";
    template_token_t tokens[] = {
        {"geomag_g", geomag_g_text},
        {"ham_solarflux", solarflux_text},
        {"ham_aindex", aindex_text},
        {"ham_kindex", kindex_text},
        {"ham_geomagfield", ham->geomagfield},
        {"ham_xray", ham->xray},
        {"ham_sunspots", sunspots_text},
        {"ham_muf", ham->muf},
        {"ham_signalnoise", ham->signalnoise},
        {"ham_aurora", aurora_text},
        {"ham_solarwind", solarwind_text},
        {"ham_magneticfield", magneticfield_text}
    };

    if (!ham->valid) {
        app_render_template(section, section_len,
            template_or_default(settings->section_template_solar_unavailable, "太阳与地磁状态暂不可用。"),
            tokens, sizeof(tokens) / sizeof(tokens[0]));
        inline_copy(summary, summary_len, section);
        return;
    }

    snprintf(geomag_g_text, sizeof(geomag_g_text), "%d", geomag_g_from_k(ham->kindex));
    snprintf(solarflux_text, sizeof(solarflux_text), "%d", ham->solarflux);
    snprintf(aindex_text, sizeof(aindex_text), "%d", ham->aindex);
    snprintf(kindex_text, sizeof(kindex_text), "%d", ham->kindex);
    snprintf(sunspots_text, sizeof(sunspots_text), "%d", ham->sunspots);
    snprintf(aurora_text, sizeof(aurora_text), "%d", ham->aurora);
    snprintf(solarwind_text, sizeof(solarwind_text), "%.1f", ham->solarwind);
    snprintf(magneticfield_text, sizeof(magneticfield_text), "%.1f", ham->magneticfield);

    app_render_template(section, section_len,
        template_or_default(settings->section_template_solar,
            "太阳通量 {{ham_solarflux}}，A={{ham_aindex}}，K={{ham_kindex}}，地磁 {{ham_geomagfield}}，X-Ray {{ham_xray}}，黑子 {{ham_sunspots}}，MUF {{ham_muf}}。\n当前地磁暴等级 G{{geomag_g}}，太阳风 {{ham_solarwind}} km/s，磁场 {{ham_magneticfield}} nT，极光指数 {{ham_aurora}}，噪声 {{ham_signalnoise}}。"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
    inline_copy(summary, summary_len, section);
}

static void build_meteor_section_custom(const settings_t *settings, const meteor_data_t *meteor, char *out, size_t out_len) {
    char meteor_name_cn[MAX_TEXT] = "";
    char peak_date_cn[MAX_TEXT] = "";
    char days_left_text[32] = "";
    char countdown_text[MAX_TEXT] = "";
    char moon_percent_text[32] = "";
    char countdown_code[MAX_TEXT] = "";
    template_token_t tokens[] = {
        {"meteor_name", meteor->shower_name},
        {"meteor_name_cn", meteor_name_cn},
        {"peak_date", meteor->peak_label},
        {"peak_date_cn", peak_date_cn},
        {"days_left", days_left_text},
        {"meteor_days_left", days_left_text},
        {"countdown_text", countdown_text},
        {"meteor_countdown_code", countdown_code},
        {"moon_percent", moon_percent_text},
        {"meteor_summary", meteor->summary},
        {"meteor_source_url", meteor->source_url}
    };

    copy_string(meteor_name_cn, sizeof(meteor_name_cn), meteor_name_to_chinese(meteor->shower_name));
    format_peak_date_cn_text(meteor->peak_label, peak_date_cn, sizeof(peak_date_cn));
    snprintf(days_left_text, sizeof(days_left_text), "%d", meteor->days_left);
    format_meteor_countdown_text(meteor->days_left, countdown_text, sizeof(countdown_text));
    snprintf(moon_percent_text, sizeof(moon_percent_text), "%d", meteor->moon_percent);
    copy_string(countdown_code, sizeof(countdown_code), meteor_countdown_code_from_days(meteor->days_left));

    if (!meteor->valid || !meteor->shower_name[0] || !meteor->peak_label[0]) {
        app_render_template(out, out_len,
            template_or_default(settings->compact_template_meteor_unavailable,
                meteor->summary[0] ? meteor->summary : "流星雨倒计时暂不可用。"),
            tokens, sizeof(tokens) / sizeof(tokens[0]));
        return;
    }

    app_render_template(out, out_len,
        template_or_default(settings->compact_template_meteor,
            "流星雨倒计时：{{meteor_name_cn}}\n峰值日期：{{peak_date_cn}}\n倒计时：{{countdown_text}}"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_satellite_section_custom(const settings_t *settings, const satellite_summary_t *satellite, char *out, size_t out_len) {
    char api_configured_text[8] = "";
    char selected_text[32] = "";
    char api_success_text[32] = "";
    char api_request_text[32] = "";
    char pass_count_text[32] = "";
    char api_status_code[MAX_TEXT] = "";
    template_token_t tokens[] = {
        {"satellite_api_configured", api_configured_text},
        {"satellite_selected_count", selected_text},
        {"satellite_api_successes", api_success_text},
        {"satellite_api_requests", api_request_text},
        {"satellite_pass_count", pass_count_text},
        {"satellite_api_status", satellite->api_status},
        {"satellite_api_status_code", api_status_code},
        {"satellite_selected_names", satellite->selected_names},
        {"satellite_summary", satellite->summary},
        {"satellite_source_url", satellite->source_url}
    };

    snprintf(api_configured_text, sizeof(api_configured_text), "%d", satellite->api_configured ? 1 : 0);
    snprintf(selected_text, sizeof(selected_text), "%d", satellite->selected_satellites);
    snprintf(api_success_text, sizeof(api_success_text), "%d", satellite->api_successes);
    snprintf(api_request_text, sizeof(api_request_text), "%d", satellite->api_requests);
    snprintf(pass_count_text, sizeof(pass_count_text), "%d", satellite->pass_count);
    copy_string(api_status_code, sizeof(api_status_code), satellite_api_status_code_from_label(satellite->api_status));

    if (!satellite->valid) {
        app_render_template(out, out_len,
            template_or_default(settings->section_template_satellite_unavailable, "卫星星历暂不可用。"),
            tokens, sizeof(tokens) / sizeof(tokens[0]));
        return;
    }

    app_render_template(out, out_len,
        template_or_default(settings->section_template_satellite, "{{satellite_summary}}"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_sixm_section_custom(const settings_t *settings, const snapshot_t *snapshot, char *out, size_t out_len) {
    char mqtt_connected_text[8] = "";
    char global_spots_15m_text[32] = "";
    char global_spots_60m_text[32] = "";
    char local_spots_15m_text[32] = "";
    char local_spots_60m_text[32] = "";
    char hamalert_hits_15m_text[32] = "";
    char hamalert_hits_60m_text[32] = "";
    char longest_path_text[32] = "";
    char best_snr_text[32] = "";
    char score_text[32] = "";
    char sixm_alert_num_text[32] = "";
    char assessment_code[MAX_TEXT] = "";
    char confidence_code[MAX_TEXT] = "";
    char sixm_alert_code[MAX_TEXT] = "";
    template_token_t tokens[] = {
        {"psk_mqtt_connected", mqtt_connected_text},
        {"psk_global_spots_15m", global_spots_15m_text},
        {"psk_global_spots_60m", global_spots_60m_text},
        {"psk_local_spots_15m", local_spots_15m_text},
        {"psk_local_spots_60m", local_spots_60m_text},
        {"psk_hamalert_hits_15m", hamalert_hits_15m_text},
        {"psk_hamalert_hits_60m", hamalert_hits_60m_text},
        {"psk_hamalert_latest_time", snapshot->psk.hamalert_latest_time},
        {"psk_hamalert_latest_text", snapshot->psk.hamalert_latest_text},
        {"psk_hamalert_sources", snapshot->psk.hamalert_sources},
        {"psk_hamalert_matched_grids", snapshot->psk.hamalert_matched_grids},
        {"psk_hamalert_matched_ols", snapshot->psk.hamalert_matched_ols},
        {"psk_longest_path_km", longest_path_text},
        {"psk_best_snr", best_snr_text},
        {"psk_score", score_text},
        {"psk_assessment", snapshot->psk.assessment},
        {"psk_assessment_code", assessment_code},
        {"psk_confidence", snapshot->psk.confidence},
        {"psk_confidence_code", confidence_code},
        {"psk_latest_pair", snapshot->psk.latest_pair},
        {"psk_latest_local_time", snapshot->psk.latest_local_time},
        {"psk_matched_grids", snapshot->psk.matched_grids},
        {"psk_farthest_peer", snapshot->psk.farthest_peer},
        {"psk_farthest_grid", snapshot->psk.farthest_grid},
        {"sixm_alert_level", sixm_alert_label_from_snapshot(snapshot, settings)},
        {"sixm_alert_level_num", sixm_alert_num_text},
        {"sixm_alert_code", sixm_alert_code}
    };

    snprintf(mqtt_connected_text, sizeof(mqtt_connected_text), "%d", snapshot->psk.mqtt_connected ? 1 : 0);
    snprintf(global_spots_15m_text, sizeof(global_spots_15m_text), "%d", snapshot->psk.global_spots_15m);
    snprintf(global_spots_60m_text, sizeof(global_spots_60m_text), "%d", snapshot->psk.global_spots_60m);
    snprintf(local_spots_15m_text, sizeof(local_spots_15m_text), "%d", snapshot->psk.local_spots_15m);
    snprintf(local_spots_60m_text, sizeof(local_spots_60m_text), "%d", snapshot->psk.local_spots_60m);
    snprintf(hamalert_hits_15m_text, sizeof(hamalert_hits_15m_text), "%d", snapshot->psk.hamalert_hits_15m);
    snprintf(hamalert_hits_60m_text, sizeof(hamalert_hits_60m_text), "%d", snapshot->psk.hamalert_hits_60m);
    snprintf(longest_path_text, sizeof(longest_path_text), "%d", snapshot->psk.longest_path_km);
    snprintf(best_snr_text, sizeof(best_snr_text), "%d", snapshot->psk.best_snr);
    snprintf(score_text, sizeof(score_text), "%d", snapshot->psk.score);
    snprintf(sixm_alert_num_text, sizeof(sixm_alert_num_text), "%d", sixm_alert_num_from_snapshot(snapshot, settings));
    copy_string(assessment_code, sizeof(assessment_code), psk_assessment_code_from_summary(&snapshot->psk));
    copy_string(confidence_code, sizeof(confidence_code), psk_confidence_code_from_summary(&snapshot->psk));
    copy_string(sixm_alert_code, sizeof(sixm_alert_code), sixm_alert_code_from_snapshot(snapshot, settings));

    app_render_template(out, out_len,
        template_or_default(settings->section_template_6m,
            "PSKReporter：15 分钟本地 {{psk_local_spots_15m}} 条，60 分钟本地 {{psk_local_spots_60m}} 条，60 分钟全球 {{psk_global_spots_60m}} 条，判断 {{psk_assessment}}，置信度 {{psk_confidence}}，分值 {{psk_score}}/100。\nHamAlert：15 分钟 {{psk_hamalert_hits_15m}} 条，60 分钟 {{psk_hamalert_hits_60m}} 条，最近一条 {{psk_hamalert_latest_text}} @ {{psk_hamalert_latest_time}}。\n监控网格命中：{{psk_matched_grids}}\n最远相关路径：{{psk_farthest_peer}} {{psk_farthest_grid}}，约 {{psk_longest_path_km}} km。\n综合提醒级别：{{sixm_alert_level}}。"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_twom_section_custom(const settings_t *settings, const snapshot_t *snapshot, char *out, size_t out_len) {
    char mqtt_connected_text[8] = "";
    char global_spots_15m_text[32] = "";
    char global_spots_60m_text[32] = "";
    char local_spots_15m_text[32] = "";
    char local_spots_60m_text[32] = "";
    char hamalert_hits_15m_text[32] = "";
    char hamalert_hits_60m_text[32] = "";
    char longest_path_text[32] = "";
    char best_snr_text[32] = "";
    char score_text[32] = "";
    char twom_alert_num_text[32] = "";
    char assessment_code[MAX_TEXT] = "";
    char confidence_code[MAX_TEXT] = "";
    char twom_alert_code[MAX_TEXT] = "";
    template_token_t tokens[] = {
        {"twom_mqtt_connected", mqtt_connected_text},
        {"twom_global_spots_15m", global_spots_15m_text},
        {"twom_global_spots_60m", global_spots_60m_text},
        {"twom_local_spots_15m", local_spots_15m_text},
        {"twom_local_spots_60m", local_spots_60m_text},
        {"twom_hamalert_hits_15m", hamalert_hits_15m_text},
        {"twom_hamalert_hits_60m", hamalert_hits_60m_text},
        {"twom_hamalert_latest_time", snapshot->twom.hamalert_latest_time},
        {"twom_hamalert_latest_text", snapshot->twom.hamalert_latest_text},
        {"twom_hamalert_sources", snapshot->twom.hamalert_sources},
        {"twom_hamalert_matched_grids", snapshot->twom.hamalert_matched_grids},
        {"twom_hamalert_matched_ols", snapshot->twom.hamalert_matched_ols},
        {"twom_longest_path_km", longest_path_text},
        {"twom_best_snr", best_snr_text},
        {"twom_score", score_text},
        {"twom_assessment", snapshot->twom.assessment},
        {"twom_assessment_code", assessment_code},
        {"twom_confidence", snapshot->twom.confidence},
        {"twom_confidence_code", confidence_code},
        {"twom_latest_pair", snapshot->twom.latest_pair},
        {"twom_latest_local_time", snapshot->twom.latest_local_time},
        {"twom_matched_grids", snapshot->twom.matched_grids},
        {"twom_farthest_peer", snapshot->twom.farthest_peer},
        {"twom_farthest_grid", snapshot->twom.farthest_grid},
        {"twom_alert_level", twom_alert_label_from_snapshot(snapshot, settings)},
        {"twom_alert_level_num", twom_alert_num_text},
        {"twom_alert_code", twom_alert_code}
    };

    snprintf(mqtt_connected_text, sizeof(mqtt_connected_text), "%d", snapshot->twom.mqtt_connected ? 1 : 0);
    snprintf(global_spots_15m_text, sizeof(global_spots_15m_text), "%d", snapshot->twom.global_spots_15m);
    snprintf(global_spots_60m_text, sizeof(global_spots_60m_text), "%d", snapshot->twom.global_spots_60m);
    snprintf(local_spots_15m_text, sizeof(local_spots_15m_text), "%d", snapshot->twom.local_spots_15m);
    snprintf(local_spots_60m_text, sizeof(local_spots_60m_text), "%d", snapshot->twom.local_spots_60m);
    snprintf(hamalert_hits_15m_text, sizeof(hamalert_hits_15m_text), "%d", snapshot->twom.hamalert_hits_15m);
    snprintf(hamalert_hits_60m_text, sizeof(hamalert_hits_60m_text), "%d", snapshot->twom.hamalert_hits_60m);
    snprintf(longest_path_text, sizeof(longest_path_text), "%d", snapshot->twom.longest_path_km);
    snprintf(best_snr_text, sizeof(best_snr_text), "%d", snapshot->twom.best_snr);
    snprintf(score_text, sizeof(score_text), "%d", snapshot->twom.score);
    snprintf(twom_alert_num_text, sizeof(twom_alert_num_text), "%d", twom_alert_num_from_snapshot(snapshot, settings));
    copy_string(assessment_code, sizeof(assessment_code), psk_assessment_code_from_summary(&snapshot->twom));
    copy_string(confidence_code, sizeof(confidence_code), psk_confidence_code_from_summary(&snapshot->twom));
    copy_string(twom_alert_code, sizeof(twom_alert_code), twom_alert_code_from_snapshot(snapshot, settings));

    app_render_template(out, out_len,
        template_or_default(settings->section_template_2m,
            "PSKReporter：15 分钟本地 {{twom_local_spots_15m}} 条，60 分钟本地 {{twom_local_spots_60m}} 条，60 分钟全球 {{twom_global_spots_60m}} 条，判断 {{twom_assessment}}，置信度 {{twom_confidence}}，分值 {{twom_score}}/100。\nHamAlert：15 分钟 {{twom_hamalert_hits_15m}} 条，60 分钟 {{twom_hamalert_hits_60m}} 条，最近一条 {{twom_hamalert_latest_text}} @ {{twom_hamalert_latest_time}}。\n监控网格命中：{{twom_matched_grids}}\n最远相关路径：{{twom_farthest_peer}} {{twom_farthest_grid}}，约 {{twom_longest_path_km}} km。\n综合提醒级别：{{twom_alert_level}}。"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_sources_section_custom(const settings_t *settings, const snapshot_t *snapshot, char *out, size_t out_len) {
    char widget_cq[MAX_LARGE_TEXT + 32] = "";
    const char *ham_source_url = snapshot->hamqsl.source_url[0] ? snapshot->hamqsl.source_url : "https://www.hamqsl.com/solarxml.php";
    const char *tropo_source_url = snapshot->tropo.page_url[0] ? snapshot->tropo.page_url : settings->tropo_source_url;
    const char *meteor_source_url = snapshot->meteor.source_url[0] ? snapshot->meteor.source_url : settings->meteor_source_url;
    const char *satellite_source_url = snapshot->satellite.source_url[0] ? snapshot->satellite.source_url : settings->satellite_source_url;
    template_token_t tokens[] = {
        {"hamqsl_widget_url", settings->hamqsl_widget_url},
        {"hamqsl_widget_cq", widget_cq},
        {"ham_source_url", ham_source_url},
        {"tropo_source_url", tropo_source_url},
        {"meteor_source_url", meteor_source_url},
        {"satellite_source_url", satellite_source_url}
    };

    if (!settings->include_hamqsl_widget && !settings->include_source_urls) {
        out[0] = '\0';
        return;
    }
    if (settings->include_hamqsl_widget && settings->hamqsl_widget_url[0]) {
        snprintf(widget_cq, sizeof(widget_cq), "[CQ:image,file=%s]", settings->hamqsl_widget_url);
    }

    app_render_template(out, out_len,
        template_or_default(settings->compact_template_hamqsl_image,
            "HAMqsl 日图：{{hamqsl_widget_cq}}\nHAMqsl：{{ham_source_url}}"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

static void build_analysis_summary_custom(const settings_t *settings, const snapshot_t *snapshot, char *out, size_t out_len) {
    char tropo_score_text[32] = "";
    char weather_score_text[32] = "";
    char twom_weather_score_text[32] = "";
    char local_spots_60m_text[32] = "";
    char twom_local_spots_60m_text[32] = "";
    char sixm_alert_num_text[32] = "";
    char twom_alert_num_text[32] = "";
    char sixm_alert_code[MAX_TEXT] = "";
    char twom_alert_code[MAX_TEXT] = "";
    template_token_t tokens[] = {
        {"sun_summary", snapshot->sun_summary},
        {"psk_assessment", snapshot->psk.assessment},
        {"psk_confidence", snapshot->psk.confidence},
        {"psk_local_spots_60m", local_spots_60m_text},
        {"twom_assessment", snapshot->twom.assessment},
        {"twom_confidence", snapshot->twom.confidence},
        {"twom_local_spots_60m", twom_local_spots_60m_text},
        {"tropo_category", snapshot->tropo.valid ? snapshot->tropo.category : ""},
        {"tropo_score", tropo_score_text},
        {"weather_level", snapshot->weather.valid ? snapshot->weather.sixm_weather_level : ""},
        {"weather_score", weather_score_text},
        {"twom_weather_level", snapshot->weather.valid ? snapshot->weather.twom_weather_level : ""},
        {"twom_weather_score", twom_weather_score_text},
        {"sixm_alert_level", sixm_alert_label_from_snapshot(snapshot, settings)},
        {"sixm_alert_level_num", sixm_alert_num_text},
        {"sixm_alert_code", sixm_alert_code},
        {"twom_alert_level", twom_alert_label_from_snapshot(snapshot, settings)},
        {"twom_alert_level_num", twom_alert_num_text},
        {"twom_alert_code", twom_alert_code}
    };

    snprintf(tropo_score_text, sizeof(tropo_score_text), "%d", snapshot->tropo.score);
    snprintf(weather_score_text, sizeof(weather_score_text), "%d", snapshot->weather.sixm_weather_score);
    snprintf(twom_weather_score_text, sizeof(twom_weather_score_text), "%d", snapshot->weather.twom_weather_score);
    snprintf(local_spots_60m_text, sizeof(local_spots_60m_text), "%d", snapshot->psk.local_spots_60m);
    snprintf(twom_local_spots_60m_text, sizeof(twom_local_spots_60m_text), "%d", snapshot->twom.local_spots_60m);
    snprintf(sixm_alert_num_text, sizeof(sixm_alert_num_text), "%d", sixm_alert_num_from_snapshot(snapshot, settings));
    snprintf(twom_alert_num_text, sizeof(twom_alert_num_text), "%d", twom_alert_num_from_snapshot(snapshot, settings));
    copy_string(sixm_alert_code, sizeof(sixm_alert_code), sixm_alert_code_from_snapshot(snapshot, settings));
    copy_string(twom_alert_code, sizeof(twom_alert_code), twom_alert_code_from_snapshot(snapshot, settings));

    app_render_template(out, out_len,
        template_or_default(settings->section_template_analysis,
            "太阳面：{{sun_summary}}\n6 米综合：PSK/HamAlert 判断“{{psk_assessment}}”，F5LEN 为“{{tropo_category}}”，气象辅助为“{{weather_level}}”，当前建议级别为“{{sixm_alert_level}}”。\n2 米综合：PSK/HamAlert 判断“{{twom_assessment}}”，F5LEN 为“{{tropo_category}}”，气象辅助为“{{twom_weather_level}}”，当前建议级别为“{{twom_alert_level}}”。\n运维提示：抓取频率按独立轮询处理，手动查询不会重置周期。"),
        tokens, sizeof(tokens) / sizeof(tokens[0]));
}

void build_reports(app_t *app, snapshot_t *snapshot) {
    settings_t settings;
    memset(&settings, 0, sizeof(settings));
    if (app) {
        pthread_mutex_lock(&app->cache_mutex);
        settings = app->settings;
        pthread_mutex_unlock(&app->cache_mutex);
    }

    build_hamqsl_section_custom(&settings, &snapshot->hamqsl, snapshot->section_hamqsl, sizeof(snapshot->section_hamqsl));
    build_weather_section_custom(&settings, &snapshot->weather, snapshot->section_weather, sizeof(snapshot->section_weather));
    build_tropo_section_custom(&settings, &snapshot->tropo, snapshot->section_tropo, sizeof(snapshot->section_tropo));
    build_solar_section_custom(&settings, &snapshot->hamqsl, snapshot->sun_summary, sizeof(snapshot->sun_summary),
        snapshot->section_solar, sizeof(snapshot->section_solar));
    build_meteor_section_custom(&settings, &snapshot->meteor, snapshot->section_meteor, sizeof(snapshot->section_meteor));
    build_satellite_section_custom(&settings, &snapshot->satellite, snapshot->section_satellite, sizeof(snapshot->section_satellite));
    build_sixm_section_custom(&settings, snapshot, snapshot->section_6m, sizeof(snapshot->section_6m));
    build_twom_section_custom(&settings, snapshot, snapshot->section_2m, sizeof(snapshot->section_2m));
    build_sources_section_custom(&settings, snapshot, snapshot->section_sources, sizeof(snapshot->section_sources));
    build_analysis_summary_custom(&settings, snapshot, snapshot->analysis_summary, sizeof(snapshot->analysis_summary));

    char refreshed[64];
    char ham_solarflux[32];
    char ham_aindex[32];
    char ham_kindex[32];
    char ham_sunspots[32];
    char ham_geomag_g[32];
    char weather_score[32];
    char twom_weather_score[32];
    char tropo_score[32];
    char meteor_days_left[32];
    char ham_updated[MAX_TEXT];
    char ham_geomagfield_text[MAX_TEXT];
    char hf_day[512];
    char hf_night[512];
    char meteor_name_cn[MAX_TEXT];
    char peak_date_cn[MAX_TEXT];
    char countdown_text[MAX_TEXT];
    char moon_percent_text[32];
    char psk_mqtt_connected[8];
    char psk_global_spots_15m[32];
    char psk_global_spots_60m[32];
    char psk_local_spots_15m[32];
    char psk_local_spots_60m[32];
    char psk_hamalert_hits_15m[32];
    char psk_hamalert_hits_60m[32];
    char psk_longest_path_km[32];
    char psk_best_snr[32];
    char psk_score[32];
    char sixm_alert_level_num[32];
    char twom_mqtt_connected[8];
    char twom_global_spots_15m[32];
    char twom_global_spots_60m[32];
    char twom_local_spots_15m[32];
    char twom_local_spots_60m[32];
    char twom_hamalert_hits_15m[32];
    char twom_hamalert_hits_60m[32];
    char twom_longest_path_km[32];
    char twom_best_snr[32];
    char twom_score[32];
    char twom_alert_level_num[32];
    char psk_assessment_code[MAX_TEXT];
    char psk_confidence_code[MAX_TEXT];
    char sixm_alert_code[MAX_TEXT];
    char twom_assessment_code[MAX_TEXT];
    char twom_confidence_code[MAX_TEXT];
    char twom_alert_code[MAX_TEXT];
    char meteor_countdown_code[MAX_TEXT];
    char tropo_category_code[MAX_TEXT];
    char satellite_api_status_code[MAX_TEXT];
    format_time_local(snapshot->refreshed_at, refreshed, sizeof(refreshed));
    snprintf(ham_solarflux, sizeof(ham_solarflux), "%d", snapshot->hamqsl.solarflux);
    snprintf(ham_aindex, sizeof(ham_aindex), "%d", snapshot->hamqsl.aindex);
    snprintf(ham_kindex, sizeof(ham_kindex), "%d", snapshot->hamqsl.kindex);
    snprintf(ham_sunspots, sizeof(ham_sunspots), "%d", snapshot->hamqsl.sunspots);
    snprintf(ham_geomag_g, sizeof(ham_geomag_g), "%d", geomag_g_from_k(snapshot->hamqsl.kindex));
    snprintf(weather_score, sizeof(weather_score), "%d", snapshot->weather.sixm_weather_score);
    snprintf(twom_weather_score, sizeof(twom_weather_score), "%d", snapshot->weather.twom_weather_score);
    snprintf(tropo_score, sizeof(tropo_score), "%d", snapshot->tropo.score);
    snprintf(meteor_days_left, sizeof(meteor_days_left), "%d", snapshot->meteor.days_left);
    copy_string(ham_updated, sizeof(ham_updated), snapshot->hamqsl.valid ? snapshot->hamqsl.updated : "");
    copy_string(ham_geomagfield_text, sizeof(ham_geomagfield_text),
        snapshot->hamqsl.valid ? snapshot->hamqsl.geomagfield : "");
    build_band_summary_text(&snapshot->hamqsl, "day", hf_day, sizeof(hf_day));
    build_band_summary_text(&snapshot->hamqsl, "night", hf_night, sizeof(hf_night));
    copy_string(meteor_name_cn, sizeof(meteor_name_cn), meteor_name_to_chinese(snapshot->meteor.shower_name));
    format_peak_date_cn_text(snapshot->meteor.peak_label, peak_date_cn, sizeof(peak_date_cn));
    format_meteor_countdown_text(snapshot->meteor.days_left, countdown_text, sizeof(countdown_text));
    snprintf(moon_percent_text, sizeof(moon_percent_text), "%d", snapshot->meteor.moon_percent);
    snprintf(psk_mqtt_connected, sizeof(psk_mqtt_connected), "%d", snapshot->psk.mqtt_connected ? 1 : 0);
    snprintf(psk_global_spots_15m, sizeof(psk_global_spots_15m), "%d", snapshot->psk.global_spots_15m);
    snprintf(psk_global_spots_60m, sizeof(psk_global_spots_60m), "%d", snapshot->psk.global_spots_60m);
    snprintf(psk_local_spots_15m, sizeof(psk_local_spots_15m), "%d", snapshot->psk.local_spots_15m);
    snprintf(psk_local_spots_60m, sizeof(psk_local_spots_60m), "%d", snapshot->psk.local_spots_60m);
    snprintf(psk_hamalert_hits_15m, sizeof(psk_hamalert_hits_15m), "%d", snapshot->psk.hamalert_hits_15m);
    snprintf(psk_hamalert_hits_60m, sizeof(psk_hamalert_hits_60m), "%d", snapshot->psk.hamalert_hits_60m);
    snprintf(psk_longest_path_km, sizeof(psk_longest_path_km), "%d", snapshot->psk.longest_path_km);
    snprintf(psk_best_snr, sizeof(psk_best_snr), "%d", snapshot->psk.best_snr);
    snprintf(psk_score, sizeof(psk_score), "%d", snapshot->psk.score);
    snprintf(sixm_alert_level_num, sizeof(sixm_alert_level_num), "%d", sixm_alert_num_from_snapshot(snapshot, &settings));
    snprintf(twom_mqtt_connected, sizeof(twom_mqtt_connected), "%d", snapshot->twom.mqtt_connected ? 1 : 0);
    snprintf(twom_global_spots_15m, sizeof(twom_global_spots_15m), "%d", snapshot->twom.global_spots_15m);
    snprintf(twom_global_spots_60m, sizeof(twom_global_spots_60m), "%d", snapshot->twom.global_spots_60m);
    snprintf(twom_local_spots_15m, sizeof(twom_local_spots_15m), "%d", snapshot->twom.local_spots_15m);
    snprintf(twom_local_spots_60m, sizeof(twom_local_spots_60m), "%d", snapshot->twom.local_spots_60m);
    snprintf(twom_hamalert_hits_15m, sizeof(twom_hamalert_hits_15m), "%d", snapshot->twom.hamalert_hits_15m);
    snprintf(twom_hamalert_hits_60m, sizeof(twom_hamalert_hits_60m), "%d", snapshot->twom.hamalert_hits_60m);
    snprintf(twom_longest_path_km, sizeof(twom_longest_path_km), "%d", snapshot->twom.longest_path_km);
    snprintf(twom_best_snr, sizeof(twom_best_snr), "%d", snapshot->twom.best_snr);
    snprintf(twom_score, sizeof(twom_score), "%d", snapshot->twom.score);
    snprintf(twom_alert_level_num, sizeof(twom_alert_level_num), "%d", twom_alert_num_from_snapshot(snapshot, &settings));
    copy_string(psk_assessment_code, sizeof(psk_assessment_code), psk_assessment_code_from_summary(&snapshot->psk));
    copy_string(psk_confidence_code, sizeof(psk_confidence_code), psk_confidence_code_from_summary(&snapshot->psk));
    copy_string(sixm_alert_code, sizeof(sixm_alert_code), sixm_alert_code_from_snapshot(snapshot, &settings));
    copy_string(twom_assessment_code, sizeof(twom_assessment_code), psk_assessment_code_from_summary(&snapshot->twom));
    copy_string(twom_confidence_code, sizeof(twom_confidence_code), psk_confidence_code_from_summary(&snapshot->twom));
    copy_string(twom_alert_code, sizeof(twom_alert_code), twom_alert_code_from_snapshot(snapshot, &settings));
    copy_string(meteor_countdown_code, sizeof(meteor_countdown_code), meteor_countdown_code_from_days(snapshot->meteor.days_left));
    copy_string(tropo_category_code, sizeof(tropo_category_code), tropo_category_code_from_label(snapshot->tropo.category));
    copy_string(satellite_api_status_code, sizeof(satellite_api_status_code), satellite_api_status_code_from_label(snapshot->satellite.api_status));

    const char *sixm_label = sixm_alert_label_from_snapshot(snapshot, &settings);
    const char *twom_label = twom_alert_label_from_snapshot(snapshot, &settings);
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
        {"section_2m", snapshot->section_2m},
        {"section_solar", snapshot->section_solar},
        {"section_meteor", snapshot->section_meteor},
        {"section_satellite", snapshot->section_satellite},
        {"section_sources", snapshot->section_sources},
        {"analysis_summary", snapshot->analysis_summary},
        {"updated", ham_updated},
        {"kindex", ham_kindex},
        {"geomagfield", ham_geomagfield_text},
        {"hf_day", hf_day},
        {"hf_night", hf_night},
        {"ham_solarflux", ham_solarflux},
        {"ham_aindex", ham_aindex},
        {"ham_kindex", ham_kindex},
        {"ham_xray", snapshot->hamqsl.xray},
        {"ham_sunspots", ham_sunspots},
        {"ham_geomagfield", snapshot->hamqsl.geomagfield},
        {"ham_muf", snapshot->hamqsl.muf},
        {"weather_level", snapshot->weather.sixm_weather_level},
        {"weather_score", weather_score},
        {"twom_weather_level", snapshot->weather.twom_weather_level},
        {"twom_weather_score", twom_weather_score},
        {"tropo_category", snapshot->tropo.category},
        {"tropo_category_code", tropo_category_code},
        {"tropo_score", tropo_score},
        {"meteor_name", snapshot->meteor.shower_name},
        {"meteor_name_cn", meteor_name_cn},
        {"peak_date", snapshot->meteor.peak_label},
        {"peak_date_cn", peak_date_cn},
        {"meteor_peak", snapshot->meteor.peak_label},
        {"meteor_days_left", meteor_days_left},
        {"days_left", meteor_days_left},
        {"countdown_text", countdown_text},
        {"meteor_countdown_code", meteor_countdown_code},
        {"moon_percent", moon_percent_text},
        {"hamqsl_widget_url", settings.hamqsl_widget_url},
        {"geomag_g", ham_geomag_g},
        {"sixm_alert_level", sixm_label},
        {"sixm_alert_level_num", sixm_alert_level_num},
        {"sixm_alert_code", sixm_alert_code},
        {"twom_alert_level", twom_label},
        {"twom_alert_level_num", twom_alert_level_num},
        {"twom_alert_code", twom_alert_code},
        {"psk_mqtt_connected", psk_mqtt_connected},
        {"psk_global_spots_15m", psk_global_spots_15m},
        {"psk_global_spots_60m", psk_global_spots_60m},
        {"psk_local_spots_15m", psk_local_spots_15m},
        {"psk_local_spots_60m", psk_local_spots_60m},
        {"psk_hamalert_hits_15m", psk_hamalert_hits_15m},
        {"psk_hamalert_hits_60m", psk_hamalert_hits_60m},
        {"psk_longest_path_km", psk_longest_path_km},
        {"psk_best_snr", psk_best_snr},
        {"psk_score", psk_score},
        {"psk_assessment", snapshot->psk.assessment},
        {"psk_assessment_code", psk_assessment_code},
        {"psk_confidence", snapshot->psk.confidence},
        {"psk_confidence_code", psk_confidence_code},
        {"psk_latest_pair", snapshot->psk.latest_pair},
        {"psk_latest_local_time", snapshot->psk.latest_local_time},
        {"psk_matched_grids", snapshot->psk.matched_grids},
        {"psk_hamalert_latest_time", snapshot->psk.hamalert_latest_time},
        {"psk_hamalert_latest_text", snapshot->psk.hamalert_latest_text},
        {"psk_hamalert_sources", snapshot->psk.hamalert_sources},
        {"psk_hamalert_matched_grids", snapshot->psk.hamalert_matched_grids},
        {"psk_hamalert_matched_ols", snapshot->psk.hamalert_matched_ols},
        {"psk_farthest_peer", snapshot->psk.farthest_peer},
        {"psk_farthest_grid", snapshot->psk.farthest_grid},
        {"twom_mqtt_connected", twom_mqtt_connected},
        {"twom_global_spots_15m", twom_global_spots_15m},
        {"twom_global_spots_60m", twom_global_spots_60m},
        {"twom_local_spots_15m", twom_local_spots_15m},
        {"twom_local_spots_60m", twom_local_spots_60m},
        {"twom_hamalert_hits_15m", twom_hamalert_hits_15m},
        {"twom_hamalert_hits_60m", twom_hamalert_hits_60m},
        {"twom_longest_path_km", twom_longest_path_km},
        {"twom_best_snr", twom_best_snr},
        {"twom_score", twom_score},
        {"twom_assessment", snapshot->twom.assessment},
        {"twom_assessment_code", twom_assessment_code},
        {"twom_confidence", snapshot->twom.confidence},
        {"twom_confidence_code", twom_confidence_code},
        {"twom_latest_pair", snapshot->twom.latest_pair},
        {"twom_latest_local_time", snapshot->twom.latest_local_time},
        {"twom_matched_grids", snapshot->twom.matched_grids},
        {"twom_hamalert_latest_time", snapshot->twom.hamalert_latest_time},
        {"twom_hamalert_latest_text", snapshot->twom.hamalert_latest_text},
        {"twom_hamalert_sources", snapshot->twom.hamalert_sources},
        {"twom_hamalert_matched_grids", snapshot->twom.hamalert_matched_grids},
        {"twom_hamalert_matched_ols", snapshot->twom.hamalert_matched_ols},
        {"twom_farthest_peer", snapshot->twom.farthest_peer},
        {"twom_farthest_grid", snapshot->twom.farthest_grid},
        {"ham_source_url", snapshot->hamqsl.source_url},
        {"tropo_page_url", snapshot->tropo.page_url},
        {"tropo_image_url", snapshot->tropo.image_url},
        {"meteor_source_url", snapshot->meteor.source_url},
        {"satellite_api_status", snapshot->satellite.api_status},
        {"satellite_api_status_code", satellite_api_status_code},
        {"satellite_selected_names", snapshot->satellite.selected_names},
        {"satellite_summary", snapshot->satellite.summary},
        {"satellite_source_url", snapshot->satellite.source_url}
    };
    size_t token_count = sizeof(tokens) / sizeof(tokens[0]);

    render_template(snapshot->report_text, sizeof(snapshot->report_text), settings.report_template_full, tokens, token_count);
    render_template(snapshot->report_6m, sizeof(snapshot->report_6m), settings.report_template_6m, tokens, token_count);
    render_template(snapshot->report_2m, sizeof(snapshot->report_2m), settings.report_template_2m, tokens, token_count);
    render_template(snapshot->report_solar, sizeof(snapshot->report_solar), settings.report_template_solar, tokens, token_count);
    render_template(snapshot->report_geomag, sizeof(snapshot->report_geomag), settings.report_template_geomag, tokens, token_count);
    render_template(snapshot->report_open6m, sizeof(snapshot->report_open6m), settings.report_template_open6m, tokens, token_count);
    render_template(snapshot->report_open2m, sizeof(snapshot->report_open2m), settings.report_template_open2m, tokens, token_count);
    render_template(snapshot->report_help, sizeof(snapshot->report_help), settings.help_template, tokens, token_count);
}
