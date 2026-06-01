/**
 * quadrature-cli - Unified CLI for Quadrature
 *
 * Usage:
 *   quadrature-cli <command> [args...]
 *
 * Commands:
 *   indexer    Manage the music library indexer
 *   setup-rt   Check and configure realtime audio
 *   help       Show help
 *   version    Show version
 */

#include <stdio.h>
#include <string.h>

#include "internal.h"

#define QUADRATURE_CLI_VERSION "0.1.0"

static void print_usage(void) {
    printf("Usage: quadrature-cli <command> [args...]\n"
           "\n"
           "Commands:\n"
           "  indexer     Manage the music library indexer\n"
           "  setup-rt    Check and configure realtime audio\n"
           "  help        Show this help\n"
           "  version     Show version\n"
           "\n"
           "Run 'quadrature-cli <command> --help' for command-specific help.\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char* cmd = argv[1];

    /* Shift argv so subcommand sees itself as argv[0] */
    int sub_argc = argc - 1;
    char** sub_argv = argv + 1;

    if (strcmp(cmd, "indexer") == 0)     return cli_indexer(sub_argc, sub_argv);
    if (strcmp(cmd, "setup-rt") == 0)    return cli_setup_rt(sub_argc, sub_argv);

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("quadrature-cli %s\n", QUADRATURE_CLI_VERSION);
        return 0;
    }

    fprintf(stderr, "Unknown command: %s\n\n", cmd);
    print_usage();
    return 1;
}
