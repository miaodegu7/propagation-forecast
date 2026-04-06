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

static const char *pick_report_kind(snapshot_t *snapshot, const char *slot_name) {
    if (strcmp(slot_name, "morning") == 0) {
        return snapshot->report_text;
    }
    return snapshot->report_text;
}

static void maybe_fire_slot(app_t *app, const char *slot_name, int enabled, const char *hhmm) {
    if (!enabled || parse_hhmm(hhmm) < 0) {
        return;
    }
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int minute_of_day = tm_local.tm_hour * 60 + tm_local.tm_min;
    if (minute_of_day != parse_hhmm(hhmm)) {
        return;
    }

    char today[32];
    format_iso_date_local(now, today, sizeof(today));
    char last_fire[32];
    storage_get_last_fire(app, slot_name, last_fire, sizeof(last_fire));
    if (strcmp(last_fire, today) == 0) {
        return;
    }

    refresh_snapshot(app, 1);
    pthread_mutex_lock(&app->cache_mutex);
    snapshot_t snapshot = app->snapshot;
    pthread_mutex_unlock(&app->cache_mutex);
    const char *report = pick_report_kind(&snapshot, slot_name);
    if (send_report_to_all_targets(app, report) >= 0) {
        storage_set_last_fire(app, slot_name, today);
        app_log(app, "INFO", "已完成 %s 定时推送", slot_name);
    }
}

static void *scheduler_thread(void *arg) {
    app_t *app = (app_t *)arg;
    while (app->running) {
        settings_t settings;
        storage_load_settings(app, &settings);
        apply_timezone(settings.timezone);
        pthread_mutex_lock(&app->cache_mutex);
        app->settings = settings;
        pthread_mutex_unlock(&app->cache_mutex);

        refresh_snapshot(app, 0);
        maybe_fire_slot(app, "morning", settings.morning_enabled, settings.schedule_morning);
        maybe_fire_slot(app, "evening", settings.evening_enabled, settings.schedule_evening);

        for (int i = 0; i < 30 && app->running; ++i) {
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

    if (storage_init(&app, db_path) != 0) {
        fprintf(stderr, "failed to initialize database: %s\n", db_path);
        return 1;
    }

    storage_load_settings(&app, &app.settings);
    apply_timezone(app.settings.timezone);
    g_app = &app;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    app_log(&app, "INFO", "服务启动，数据库: %s", db_path);
    psk_start(&app);
    refresh_snapshot(&app, 1);

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

    return server_rc == 0 ? 0 : 1;
}
