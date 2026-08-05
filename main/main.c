#include "app.h"

#include "api/aosl_log.h"
#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_time.h>

#include <string.h>
#include <stdlib.h>
#include <signal.h>

/* ----------------------------------------------------------
 * Platform-specific audio backend (Linux: ALSA).
 * Registers the capture/playback ops used by the app layer via
 * audio_device_register_*() before mybot_app_start() is called.
 * ---------------------------------------------------------- */
void mybot_audio_platform_register_alsa_capture(void);
void mybot_audio_platform_register_alsa_playback(void);

static volatile sig_atomic_t s_exit_requested;

/* ----------------------------------------------------------
 * Signal handling — POSIX. Request a graceful app exit.
 * ---------------------------------------------------------- */
static void signal_handler(int sig)
{
    (void)sig;
    /* Only sig_atomic_t access is performed in the signal handler. */
    s_exit_requested = 1;
}

/* ----------------------------------------------------------
 * Interactive keys (stdin) — maps to app-level requests.
 * ---------------------------------------------------------- */
static void handle_key(char ch)
{
    switch (ch) {
    case 's':
        AOSL_LOG_INF("[KEY] s -> start conversation");
        mybot_app_start_conversation();
        break;
    case 'q':
        AOSL_LOG_INF("[KEY] q -> stop conversation");
        mybot_app_stop_conversation();
        break;
    case 'p':
        AOSL_LOG_INF("[KEY] p -> re-pair");
        mybot_app_pair();
        break;
    case 'e':
        AOSL_LOG_INF("[KEY] e -> exit");
        mybot_app_request_exit();
        break;
    case '\n':
    case '\r':
        break;
    default:
        AOSL_LOG_INF("[KEY] '%c' ignored (s=start, q=stop, p=pair, e=exit)", ch);
        break;
    }
}

static void print_usage(const char *prog)
{
    AOSL_LOG_INF(
        "Usage: %s --server <URL> --device-id <ID> [options]\n"
        "\n"
        "Required:\n"
        "  --server <url>     Device API server base URL\n"
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

int main(int argc, char **argv)
{
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
    if (cfg.firmware_ver[0]) { AOSL_LOG_INF("  fw-ver   : %s", cfg.firmware_ver); }
    if (cfg.hw_model[0]) { AOSL_LOG_INF("  hw-model : %s", cfg.hw_model); }

    /* ---- Register the platform audio backend (Linux: ALSA) ---- */
    mybot_audio_platform_register_alsa_capture();
    mybot_audio_platform_register_alsa_playback();

    /* ---- Install signal handlers ---- */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- Start the application (non-blocking) ---- */
    if (mybot_app_start(&cfg) < 0) {
        return 1;
    }

    /* ---- Set stdin non-blocking for interactive keys ---- */
    aosl_hal_sk_set_nonblock((aosl_fd_t)0);

    AOSL_LOG_INF("=== mybot ready ===\n"
                 "  s - start conversation\n"
                 "  q - stop conversation\n"
                 "  p - re-pair device\n"
                 "  e - exit\n"
                 "  Ctrl+C - exit");

    /* ---- Main loop: interactive keys only.
     * The app drives itself (device state machine etc.) from its own MPQ
     * timers, so main() only needs to poll stdin here. ---- */
    while (mybot_app_is_running()) {
        if (s_exit_requested) {
            mybot_app_request_exit();
            continue;
        }

        char ch;
        if (aosl_hal_sk_read((aosl_fd_t)0, &ch, 1) == 1) {
            handle_key(ch);
        }
        aosl_hal_msleep(100);
    }

    /* ---- Stop the application and release all resources ---- */
    mybot_app_stop();
    return 0;
}
