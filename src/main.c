#include "app.h"
#include <signal.h>

static app_t *g_app = NULL;

static void handle_signal(int signo) {
    if (g_app) {
        app_write_boot_log("鏀跺埌绯荤粺淇″彿: %d", signo);
        g_app->running = 0;
        if (g_app->http_fd != APP_INVALID_SOCKET) {
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
    if (tm_local.tm_hour * 60 + tm_local.tm_min != parse_hhmm(rule->hhmm)) {
        return;
    }

    char today[32];
    format_iso_date_local(now, today, sizeof(today));
    if (strcmp(rule->last_fire_date, today) == 0) {
        return;
    }

    refresh_snapshot(app, 0);

    char *report_copy = NULL;
    pthread_mutex_lock(&app->cache_mutex);
    {
        const char *report_src = app_get_report_by_kind(&app->snapshot, rule->report_kind);
        if (report_src && *report_src) {
            size_t len = strlen(report_src) + 1;
            report_copy = malloc(len);
            if (report_copy) {
                memcpy(report_copy, report_src, len);
            }
        }
    }
    pthread_mutex_unlock(&app->cache_mutex);

    if (report_copy && send_report_to_all_targets(app, report_copy) >= 0) {
        storage_set_schedule_last_fire(app, rule->id, today);
        app_log(app, "INFO", "宸叉墽琛屽畾鏃舵帹閫? %s %s", rule->label, rule->hhmm);
    }
    free(report_copy);
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
    char default_db_path[1024];
    app_default_db_path(default_db_path, sizeof(default_db_path));
    const char *db_path = default_db_path;
    int hide_console = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-browser") == 0) {
            continue;
        }
        if (strcmp(argv[i], "--show-console") == 0) {
            hide_console = 0;
            continue;
        }
        if (strcmp(argv[i], "--hide-console") == 0) {
            hide_console = 1;
            continue;
        }
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
            continue;
        }
        if (argv[i][0] != '-') {
            db_path = argv[i];
        }
    }

    app_prepare_desktop_mode(hide_console);
    app_write_boot_log("绋嬪簭鍚姩锛屾暟鎹簱璺緞: %s", db_path);

    app_t *app = calloc(1, sizeof(*app));
    if (!app) {
        app_write_boot_log("鍐呭瓨鍒嗛厤澶辫触");
        if (hide_console) {
            app_show_startup_error("浼犳挱鍚庡彴鍚姩澶辫触", "鍐呭瓨鍒嗛厤澶辫触锛岃鏌ョ湅 propagation_bot.log");
        }
        return 1;
    }
    app->running = 1;
    app->http_fd = APP_INVALID_SOCKET;
    app->open_admin_console_on_start = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-browser") == 0) {
            app->open_admin_console_on_start = 0;
        }
    }

    pthread_mutex_init(&app->db_mutex, NULL);
    pthread_mutex_init(&app->cache_mutex, NULL);
    pthread_mutex_init(&app->refresh_mutex, NULL);
    pthread_mutex_init(&app->spot_mutex, NULL);
    pthread_mutex_init(&app->hamalert_mutex, NULL);
    pthread_mutex_init(&app->rate_mutex, NULL);
    pthread_mutex_init(&app->async_mutex, NULL);

    if (app_net_init() != 0) {
        fprintf(stderr, "failed to initialize network runtime\n");
        app_write_boot_log("缃戠粶杩愯鏃跺垵濮嬪寲澶辫触");
        if (hide_console) {
            app_show_startup_error("浼犳挱鍚庡彴鍚姩澶辫触", "缃戠粶杩愯鏃跺垵濮嬪寲澶辫触锛岃鏌ョ湅 propagation_bot.log");
        }
        free(app);
        return 1;
    }

    if (storage_init(app, db_path) != 0) {
        fprintf(stderr, "failed to initialize database: %s\n", db_path);
        if (hide_console) {
            char message[1024];
            snprintf(message, sizeof(message), "%s\n\n璇锋煡鐪嬪悓鐩綍涓嬬殑 propagation_bot.log",
                app->last_error[0] ? app->last_error : "鏁版嵁搴撳垵濮嬪寲澶辫触");
            app_show_startup_error("浼犳挱鍚庡彴鍚姩澶辫触", message);
        }
        app_net_cleanup();
        free(app);
        return 1;
    }

    storage_load_settings(app, &app->settings);
    apply_timezone(app->settings.timezone);
    g_app = app;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    char temp[64];
    storage_get_state(app, "last_geomag_alert_g", temp, sizeof(temp));
    app->last_geomag_alert_g = atoi(temp);
    storage_get_state(app, "last_6m_alert_level", temp, sizeof(temp));
    app->last_sixm_alert_level = atoi(temp);
    storage_get_state(app, "last_6m_alert_at", temp, sizeof(temp));
    app->last_sixm_alert_at = (time_t)atoll(temp);
    storage_get_state(app, "last_2m_alert_level", temp, sizeof(temp));
    app->last_twom_alert_level = atoi(temp);
    storage_get_state(app, "last_2m_alert_at", temp, sizeof(temp));
    app->last_twom_alert_at = (time_t)atoll(temp);

    app_log(app, "INFO", "鏈嶅姟鍚姩锛屾暟鎹簱: %s", db_path);
    psk_start(app);
    app_rebuild_snapshot(app);
    app_request_refresh_async(app, 1, 1, "startup");

    pthread_t scheduler;
    pthread_create(&scheduler, NULL, scheduler_thread, app);

    int server_rc = http_server_run(app);
    app_log(app, server_rc == 0 ? "WARN" : "ERROR", "HTTP 鏈嶅姟涓诲惊鐜粨鏉燂紝server_rc=%d", server_rc);
    if (server_rc != 0 && hide_console && !app->admin_console_opened) {
        char message[1024];
        snprintf(message, sizeof(message), "%s\n\n璇锋煡鐪嬪悓鐩綍涓嬬殑 propagation_bot.log",
            app->last_error[0] ? app->last_error : "HTTP 鍚庡彴鍚姩澶辫触");
        app_show_startup_error("浼犳挱鍚庡彴鍚姩澶辫触", message);
    }

    app->running = 0;
    pthread_join(scheduler, NULL);
    psk_stop(app);

    sqlite3_close(app->db);
    app_net_cleanup();
    pthread_mutex_destroy(&app->db_mutex);
    pthread_mutex_destroy(&app->cache_mutex);
    pthread_mutex_destroy(&app->refresh_mutex);
    pthread_mutex_destroy(&app->spot_mutex);
    pthread_mutex_destroy(&app->hamalert_mutex);
    pthread_mutex_destroy(&app->rate_mutex);
    pthread_mutex_destroy(&app->async_mutex);
    free(app);
    return server_rc == 0 ? 0 : 1;
}
