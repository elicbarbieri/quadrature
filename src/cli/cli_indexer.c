/**
 * quadrature-cli indexer - Music library indexer subcommand
 *
 * Usage:
 *   quadrature-cli indexer start [options] <path>
 *   quadrature-cli indexer stop
 *   quadrature-cli indexer status
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <glib.h>

#include "quadrature/quadrature.h"
#include "quadrature/indexer.h"
#include "internal.h"

// =============================================================================
// Signal Handling
// =============================================================================

static volatile sig_atomic_t g_shutdown_requested = 0;
static indexer_t *g_indexer = NULL;

static void
signal_handler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
    if (g_indexer)
        indexer_cancel(g_indexer);
}

static void
install_signal_handlers(void)
{
    struct sigaction sa = { .sa_handler = signal_handler, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

// =============================================================================
// Progress Callbacks
// =============================================================================

static void
indexer_progress_callback(indexer_event_t event,
                          const indexer_progress_t *progress,
                          const library_cache_changeset_t *changeset,
                          void *user_data)
{
    (void)user_data;
    (void)changeset;

    switch (event) {
    case INDEXER_STARTED:
        printf("Indexing started...\n");
        break;

    case INDEXER_PROGRESS:
        if (progress->phase == INDEXER_PHASE_SCANNING) {
            printf("\rScanning: %zu dirs, %zu files found",
                   progress->dirs_scanned,
                   progress->files_total);
        } else if (progress->phase == INDEXER_PHASE_METADATA) {
            printf("\rMetadata: %zu/%zu files (%.1f%%)",
                   progress->files_processed,
                   progress->files_total,
                   progress->progress * 100.0);
        } else if (progress->phase == INDEXER_PHASE_ARTWORK) {
            printf("\rArtwork: %zu/%zu albums (%.1f%%)",
                   progress->albums_processed,
                   progress->albums_total,
                   progress->progress * 100.0);
        }
        fflush(stdout);
        break;

    case INDEXER_LIBRARY_UPDATED:
        printf("\nLibrary updated (cache refresh available)\n");
        break;

    case INDEXER_ARTWORK_UPDATED:
        printf("\nArtwork updated\n");
        break;

    case INDEXER_COMPLETED:
        printf("\nIndexing complete: %zu files, %zu new, %zu errors\n",
               progress->files_total,
               progress->files_new,
               progress->error_count);
        break;

    case INDEXER_CANCELLED:
        printf("\nIndexing cancelled\n");
        break;

    case INDEXER_ERROR:
        fprintf(stderr, "\nIndexer error\n");
        break;
    }
}

// =============================================================================
// Subcommands
// =============================================================================

static void
print_start_help(void)
{
    printf(
        "Usage: quadrature-cli indexer start [options] <path>\n\n"
        "Options:\n"
        "  -R, --mb-resolve                 Resolve metadata from MusicBrainz (requires "
        "--pg-conninfo)\n"
        "  -p, --pg-conninfo <str>          PostgreSQL connection string for MusicBrainz database\n"
        "  -a, --acoustid-pg-conninfo <str> PostgreSQL connection string for AcoustID database\n"
        "  -i, --acoustid-index-url <url>   AcoustID index HTTP URL (e.g. http://host:8081)\n"
        "  -S, --mb-solr-url <url>          MusicBrainz Solr URL (e.g. http://host:8983)\n"
        "  -F, --fanart-api-key <key>       fanart.tv API key for artist artwork\n"
        "  -d, --data-dir <path>            Write quadrature.sqlite + artwork to <path>\n"
        "                                    instead of the music dir (useful for diagnostics)\n"
        "  -v, --verbose                    Verbose output\n"
        "  -h, --help                       Show help\n");
}

static int
cmd_start(int argc, char **argv)
{
    const char *music_path = NULL;
    bool verbose = false;
    bool mb_resolve = false;
    const char *pg_conninfo = NULL;
    const char *acoustid_pg_conninfo = NULL;
    const char *acoustid_index_url = NULL;
    const char *mb_solr_url = NULL;
    const char *fanart_api_key = NULL;
    const char *data_dir = NULL;

    static struct option long_options[] = { { "verbose", no_argument, 0, 'v' },
                                            { "help", no_argument, 0, 'h' },
                                            { "mb-resolve", no_argument, 0, 'R' },
                                            { "pg-conninfo", required_argument, 0, 'p' },
                                            { "acoustid-pg-conninfo", required_argument, 0, 'a' },
                                            { "acoustid-index-url", required_argument, 0, 'i' },
                                            { "mb-solr-url", required_argument, 0, 'S' },
                                            { "fanart-api-key", required_argument, 0, 'F' },
                                            { "data-dir", required_argument, 0, 'd' },
                                            { 0, 0, 0, 0 } };

    optind = 1; /* reset getopt for subcommand parsing */
    int opt;
    while ((opt = getopt_long(argc, argv, "vhRp:a:i:S:F:d:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'v':
            verbose = true;
            break;
        case 'h':
            print_start_help();
            return 0;
        case 'R':
            mb_resolve = true;
            break;
        case 'p':
            pg_conninfo = optarg;
            break;
        case 'a':
            acoustid_pg_conninfo = optarg;
            break;
        case 'i':
            acoustid_index_url = optarg;
            break;
        case 'S':
            mb_solr_url = optarg;
            break;
        case 'F':
            fanart_api_key = optarg;
            break;
        case 'd':
            data_dir = optarg;
            break;
        default:
            print_start_help();
            return 1;
        }
    }

    if (optind < argc) {
        music_path = argv[optind];
    } else {
        fprintf(stderr, "Error: Music path required\n");
        print_start_help();
        return 1;
    }

    if (verbose) {
        g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
    }

    printf("Indexing music library\n");
    printf("  Path: %s\n\n", music_path);

    indexer_config_t config = {
        .thread_count = 0, /* auto */
        .process_artwork = true,
        .art_size = 300,
        .callback = indexer_progress_callback,
        .user_data = NULL,
        .mb_resolve = mb_resolve,
        .pg_conninfo = pg_conninfo,
        .mb_solr_url = mb_solr_url,
        .acoustid_pg_conninfo = acoustid_pg_conninfo,
        .acoustid_index_url = acoustid_index_url,
        .fetch_artist_art = true,
        .fanart_api_key = fanart_api_key,
        .fetch_artist_bios = true,
    };

    indexer_t *indexer = NULL;
    quadrature_result_t result = indexer_create(&indexer, &config);
    if (result != QUADRATURE_OK) {
        fprintf(stderr, "Failed to create indexer: %d\n", result);
        return 1;
    }

    g_indexer = indexer;
    install_signal_handlers();

    result = indexer_scan(indexer, music_path, data_dir);
    if (result != QUADRATURE_OK) {
        fprintf(stderr, "Failed to start indexing: %d\n", result);
        indexer_destroy(indexer);
        return 1;
    }

    indexer_wait(indexer);
    g_indexer = NULL;

    indexer_destroy(indexer);

    return g_shutdown_requested ? 130 : 0;
}

static int
cmd_stop(void)
{
    fprintf(stderr, "Indexer daemon not yet implemented.\n");
    return 1;
}

static int
cmd_status(void)
{
    fprintf(stderr, "Indexer daemon not yet implemented.\n");
    return 1;
}

// =============================================================================
// Entry Point
// =============================================================================

static void
print_help(void)
{
    printf("Usage: quadrature-cli indexer <subcommand> [args...]\n\n"
           "Subcommands:\n"
           "  start [options] <path>  Run indexer scan on a music library\n"
           "  stop                    Stop the indexer daemon\n"
           "  status                  Show indexer daemon status\n");
}

int
cli_indexer(int argc, char **argv)
{
    if (argc < 2) {
        print_help();
        return 1;
    }

    const char *subcmd = argv[1];

    if (g_strcmp0(subcmd, "start") == 0)
        return cmd_start(argc - 1, argv + 1);
    if (g_strcmp0(subcmd, "stop") == 0)
        return cmd_stop();
    if (g_strcmp0(subcmd, "status") == 0)
        return cmd_status();

    if (g_strcmp0(subcmd, "help") == 0 || g_strcmp0(subcmd, "--help") == 0
        || g_strcmp0(subcmd, "-h") == 0) {
        print_help();
        return 0;
    }

    fprintf(stderr, "Unknown indexer subcommand: %s\n\n", subcmd);
    print_help();
    return 1;
}
