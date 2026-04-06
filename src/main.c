#include "app.h"
#include <signal.h>

static app_t *g_app = NULL;

static void handle_signal(int signo) {
    if (g_app) {
        g_app->running = 0;
        if (g_app->http_fd > 0) {
            shutdown(g_app->http_fd, SHUT_RDWR);
        }
    }
    (void)signo;
}

static void maybe_fire_schedule_rule(app_t *app, const schedule_rule_t *rule) {
    if (!rule->enabled || parse_hhmm(rule->hhmm) < 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int minute_of_day = tm_local.tm_hour * 60 + tm_local.tm_min;
    if (minute_of_day != parse_hhmm(rule->hhmm)) {
        return;
    }

    char today[32];
    format_iso_date_local(now, today, sizeof(today));
    if (strcmp(rule->last_fire_date, today) == 0) {
        return;
    }

    refresh_snapshot(app, 0);
    pthread_mutex_lock(&app->cache_mutex);
    snapshot_t snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);
    const char *report = app_get_report_by_kind(&snapshot, rule->report_kind);
    if (send_report_to_all_targets(app, report) >= 0) {
        storage_set_schedule_last_fire(app, rule->id, today);
        app_log(app, "INFO", "已执行定时推送: %s %s", rule->label, rule->hhmm);
    }
}

static void *scheduler_thread(void *arg) {
    app_t *app = (app_t *)arg;
    while (app->running) {
        app_run_periodic_fetches(app);
        app_check_alerts(app);

        schedule_rule_t rules[MAX_SCHEDULES];
        int count = 0;
        storage_load_schedules(app, rules, MAX_SCHEDULES, &count);
        for (int i = 0; i < count; ++i) {
            maybe_fire_schedule_rule(app, &rules[i]);
        }

        for (int i = 0; i < 5 && app->running; ++i) {
            sleep(1);
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *db_path = argc > 1 ? argv[1] : "./propagation.db";
    app_t app;
    memset(&app, 0, sizeof(app));
    app.running = 1;
    app.http_fd = -1;

    pthread_mutex_init(&app.db_mutex, NULL);
    pthread_mutex_init(&app.cache_mutex, NULL);
    pthread_mutex_init(&app.refresh_mutex, NULL);
    pthread_mutex_init(&app.spot_mutex, NULL);
    pthread_mutex_init(&app.rate_mutex, NULL);

    if (storage_init(&app, db_path) != 0) {
        fprintf(stderr, "failed to initialize database: %s\n", db_path);
        return 1;
    }

    storage_load_settings(&app, &app.settings);
    apply_timezone(app.settings.timezone);
    g_app = &app;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    char temp[64];
    storage_get_state(&app, "last_geomag_alert_g", temp, sizeof(temp));
    app.last_geomag_alert_g = atoi(temp);
    storage_get_state(&app, "last_6m_alert_level", temp, sizeof(temp));
    app.last_sixm_alert_level = atoi(temp);
    storage_get_state(&app, "last_6m_alert_at", temp, sizeof(temp));
    app.last_sixm_alert_at = (time_t)atoll(temp);

    app_log(&app, "INFO", "服务启动，数据库: %s", db_path);
    psk_start(&app);
    app_force_refresh(&app);

    pthread_t scheduler;
    pthread_create(&scheduler, NULL, scheduler_thread, &app);

    int server_rc = http_server_run(&app);
    app.running = 0;
    pthread_join(scheduler, NULL);
    psk_stop(&app);

    sqlite3_close(app.db);
    pthread_mutex_destroy(&app.db_mutex);
    pthread_mutex_destroy(&app.cache_mutex);
    pthread_mutex_destroy(&app.refresh_mutex);
    pthread_mutex_destroy(&app.spot_mutex);
    pthread_mutex_destroy(&app.rate_mutex);

    return server_rc == 0 ? 0 : 1;
}
