#include "app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
    fprintf(stdout, "Press Ctrl+C to stop\n\n");

    return app_start(&cfg);
}
