#include "mybot_app.h"

#include "api/aosl_log.h"
#include <hal/aosl_hal_time.h>

#include <string.h>
#include <stdlib.h>
#include <signal.h>

/* ----------------------------------------------------------
 * Platform backends (Linux: host network, ALSA, file-backed storage, stdin keys, and console LCD).
 * Registers the platform ops used by the app layer before
 * mybot_app_start() is called.
 * ---------------------------------------------------------- */
void mybot_audio_platform_register_alsa_capture(void);
void mybot_audio_platform_register_alsa_playback(void);
void mybot_kv_store_platform_register_file(void);
void mybot_key_platform_register_stdin(void);
void mybot_lcd_platform_register_console(void);
void mybot_wifi_platform_register_host_network(void);
static volatile sig_atomic_t s_exit_requested;

/* ----------------------------------------------------------
 * Signal handling — POSIX. Request a graceful app exit.
 * ---------------------------------------------------------- */
static void signal_handler(int sig) {
    (void)sig;
    /* Only sig_atomic_t access is performed in the signal handler. */
    s_exit_requested = 1;
}

static void print_usage(const char *prog) {
    AOSL_LOG_INF("Usage: %s --server <URL> --device-id <ID> [options]\n"
                 "\n"
                 "Required:\n"
                 "  --server <url>     Service base URL\n"
                 "                     e.g. http://localhost:3001\n"
                 "  --device-id <id>   Unique device identifier\n"
                 "                     e.g. AG-A1B2C3\n"
                 "\n"
                 "Options:\n"
                 "  --fw-ver <str>     Firmware version string (optional)\n"
                 "  --hw-model <str>   Hardware model string (optional)\n"
                 "  -h, --help         Show this help\n"
                 "\n"
                 "Example:\n"
                 "  %s --server http://localhost:3001 --device-id AG-DEMO-001",
                 prog, prog);
}

int main(int argc, char **argv) {
    mybot_app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    aosl_set_log_level(AOSL_LOG_INFO);

    /* ---- Parse command line ---- */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            strncpy(cfg.server_base, argv[++i], sizeof(cfg.server_base) - 1);
        } else if (strcmp(argv[i], "--device-id") == 0 && i + 1 < argc) {
            strncpy(cfg.device_id, argv[++i], sizeof(cfg.device_id) - 1);
        } else if (strcmp(argv[i], "--fw-ver") == 0 && i + 1 < argc) {
            strncpy(cfg.firmware_ver, argv[++i], sizeof(cfg.firmware_ver) - 1);
        } else if (strcmp(argv[i], "--hw-model") == 0 && i + 1 < argc) {
            strncpy(cfg.hw_model, argv[++i], sizeof(cfg.hw_model) - 1);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            AOSL_LOG_ERR("unknown option: %s", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cfg.server_base[0] == '\0' || cfg.device_id[0] == '\0') {
        AOSL_LOG_ERR("--server and --device-id are required");
        print_usage(argv[0]);
        return 1;
    }

    AOSL_LOG_INF("mybot v0.1.0 starting...");
    AOSL_LOG_INF("  server   : %s", cfg.server_base);
    AOSL_LOG_INF("  device-id: %s", cfg.device_id);
    if (cfg.firmware_ver[0]) {
        AOSL_LOG_INF("  fw-ver   : %s", cfg.firmware_ver);
    }
    if (cfg.hw_model[0]) {
        AOSL_LOG_INF("  hw-model : %s", cfg.hw_model);
    }

    /* ---- Register platform backends. Wi-Fi provisioning is the first app stage. ---- */
    mybot_wifi_platform_register_host_network();
    mybot_audio_platform_register_alsa_capture();
    mybot_audio_platform_register_alsa_playback();
    mybot_kv_store_platform_register_file();
    mybot_key_platform_register_stdin();
    mybot_lcd_platform_register_console();

    /* ---- Install signal handlers ---- */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- Start the application (non-blocking) ---- */
    if (mybot_app_start(&cfg) < 0) {
        return 1;
    }

    /* ---- Main loop: wait for a key event or process signal to request exit. ---- */
    bool interactive_help_printed = false;
    while (mybot_app_is_running()) {
        if (!interactive_help_printed && mybot_app_get_state() == MYBOT_APP_STATE_READY) {
            AOSL_LOG_INF("=== mybot ready ===\n"
                         "  s - start conversation\n"
                         "  q - stop conversation\n"
                         "  p - re-pair device\n"
                         "  e - exit\n"
                         "  Ctrl+C - exit");
            interactive_help_printed = true;
        }

        if (s_exit_requested) {
            mybot_app_request_exit();
            continue;
        }

        aosl_hal_msleep(100);
    }

    /* ---- Stop the application and release all resources ---- */
    mybot_app_stop();
    return 0;
}
