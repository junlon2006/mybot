#include "app.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --app_id <APP_ID> --channel <CHANNEL> [options]\n"
        "\n"
        "Options:\n"
        "  --app_id <id>      Agora App ID (required)\n"
        "  --channel <name>   Channel name (required)\n"
        "  --token <token>    Token for authentication (optional)\n"
        "  --user <name>      User account string (default: mybot_user)\n"
        "  -h, --help         Show this help\n"
        "\n"
        "Example:\n"
        "  %s --app_id abc123 --channel mybot_test --user client_a\n",
        prog, prog);
}

int main(int argc, char **argv)
{
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.user, sizeof(cfg.user), "%s", "mybot_user");

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--app_id") == 0 && i + 1 < argc) {
            strncpy(cfg.app_id, argv[++i], sizeof(cfg.app_id) - 1);
        } else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
            strncpy(cfg.channel, argv[++i], sizeof(cfg.channel) - 1);
        } else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) {
            strncpy(cfg.token, argv[++i], sizeof(cfg.token) - 1);
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            strncpy(cfg.user, argv[++i], sizeof(cfg.user) - 1);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (cfg.app_id[0] == '\0' || cfg.channel[0] == '\0') {
        fprintf(stderr, "ERROR: --app_id and --channel are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    fprintf(stdout, "mybot v0.1.0 starting...\n");
    fprintf(stdout, "  app_id : %s\n", cfg.app_id);
    fprintf(stdout, "  channel: %s\n", cfg.channel);
    if (cfg.token[0])
        fprintf(stdout, "  token  : %s\n", cfg.token);
    fprintf(stdout, "  user   : %s\n", cfg.user);
    fprintf(stdout, "Press Ctrl+C to stop\n\n");

    return app_start(&cfg);
}
