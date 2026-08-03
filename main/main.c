#include "app.h"

#include "api/aosl_log.h"
#include <hal/aosl_hal_socket.h>
#include <hal/aosl_hal_time.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

/* ----------------------------------------------------------
 * Platform-specific audio backend (Linux: ALSA).
 * Registers the capture/playback ops used by the app layer via
 * audio_device_register_*() before app_start() is called.
 * ---------------------------------------------------------- */
void audio_platform_register_alsa_capture(void);
void audio_platform_register_alsa_playback(void);

/* ----------------------------------------------------------
 * Signal handling — POSIX. Request a graceful app exit.
 * ---------------------------------------------------------- */
static void signal_handler(int sig)
{
    (void)sig;
    AOSL_LOG_INF("caught signal, stopping...");
    app_request_exit();
}

/* ----------------------------------------------------------
 * Interactive keys (stdin) — maps to app-level requests.
 * ---------------------------------------------------------- */
static void handle_key(char ch)
{
    switch (ch) {
    case 's':
        fprintf(stdout, "[KEY] s -> start conversation\n");
        app_start_conversation();
        break;
    case 'q':
        fprintf(stdout, "[KEY] q -> stop conversation\n");
        app_stop_conversation();
        break;
    case 'p':
        fprintf(stdout, "[KEY] p -> re-pair\n");
        app_pair();
        break;
    case 'e':
        fprintf(stdout, "[KEY] e -> exit\n");
        app_request_exit();
        break;
    case '\n':
    case '\r':
        break;
    default:
        fprintf(stdout, "[KEY] '%c' ignored (s=start, q=stop, p=pair, e=exit)\n", ch);
        break;
    }
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
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
        "  %s --server http://localhost:3001 --device-id AG-DEMO-001\n",
        prog, prog);
}

int main(int argc, char **argv)
{
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

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
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cfg.server_base[0] == '\0' || cfg.device_id[0] == '\0') {
        fprintf(stderr, "ERROR: --server and --device-id are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    fprintf(stdout, "mybot v0.1.0 starting...\n");
    fprintf(stdout, "  server   : %s\n", cfg.server_base);
    fprintf(stdout, "  device-id: %s\n", cfg.device_id);
    if (cfg.firmware_ver[0]) fprintf(stdout, "  fw-ver   : %s\n", cfg.firmware_ver);
    if (cfg.hw_model[0])     fprintf(stdout, "  hw-model : %s\n", cfg.hw_model);

    /* ---- Register the platform audio backend (Linux: ALSA) ---- */
    audio_platform_register_alsa_capture();
    audio_platform_register_alsa_playback();

    /* ---- Install signal handlers ---- */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- Start the application (non-blocking) ---- */
    if (app_start(&cfg) < 0) {
        fprintf(stderr, "ERROR: app_start failed\n");
        app_stop();   /* release anything app_start allocated before failing */
        return 1;
    }

    /* ---- Set stdin non-blocking for interactive keys ---- */
    aosl_hal_sk_set_nonblock((aosl_fd_t)0);

    fprintf(stdout, "\n"
        "=== mybot ready ===\n"
        "  s - start conversation\n"
        "  q - stop conversation\n"
        "  p - re-pair device\n"
        "  e - exit\n"
        "  Ctrl+C - exit\n"
        "\n");

    /* ---- Main loop: interactive keys + application tick ---- */
    while (app_is_running()) {
        char ch;
        if (aosl_hal_sk_read((aosl_fd_t)0, &ch, 1) == 1)
            handle_key(ch);
        app_tick();
        aosl_hal_msleep(100);
    }

    /* ---- Stop the application and release all resources ---- */
    app_stop();
    return 0;
}
