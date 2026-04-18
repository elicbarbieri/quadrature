#include <criterion/criterion.h>
#include <criterion/hooks.h>
#include "quadrature/quadrature.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <libavformat/avformat.h>

// Initialize FFmpeg before any tests run (before Criterion forks)
// This prevents FFmpeg internal state issues after fork
ReportHook(PRE_ALL)(struct criterion_test_set *tests) {
    (void)tests;
    avformat_network_init();
}

// Forward declarations from indexer internals
typedef struct {
    char* path;
    char* title;
    char* artist;
    char* album_artist;
    char* album;
    char* genre;
    uint32_t duration_ms;
    uint16_t track_num;
    uint16_t disc_num;
    uint16_t year;
    int64_t mtime;
    int64_t size;
} index_item_t;

extern const char* AUDIO_EXTENSIONS[];
bool is_audio_file(const char* path);
quadrature_result_t extract_audio_metadata(const char* path, index_item_t* out);
void index_item_free(index_item_t* item);

// Test library paths (relative to project root)
#define TEST_LIBRARY "tests/assets/library"
#define BACH_ALBUM   TEST_LIBRARY "/Johann Sebastian Bach/Goldberg Variations"
#define BEETHOVEN_MP3_ALBUM TEST_LIBRARY "/Ludwig van Beethoven/String Quartet No. 6"
#define BORODIN_ALBUM TEST_LIBRARY "/Alexander Borodin/String Quartet No. 1"
#define BEETHOVEN_WAV_ALBUM TEST_LIBRARY "/Ludwig van Beethoven/Symphony No. 3 Eroica"

// ============================================================================
// Audio File Detection
// ============================================================================

Test(indexer, audio_file_detection) {
    // Supported formats
    cr_assert(is_audio_file("track.mp3"));
    cr_assert(is_audio_file("track.flac"));
    cr_assert(is_audio_file("track.ogg"));
    cr_assert(is_audio_file("track.wav"));
    cr_assert(is_audio_file("track.opus"));
    cr_assert(is_audio_file("track.m4a"));
    cr_assert(is_audio_file("TRACK.FLAC"));  // Case insensitive

    // Not audio
    cr_assert(!is_audio_file("cover.jpg"));
    cr_assert(!is_audio_file("readme.txt"));
    cr_assert(!is_audio_file("video.mp4"));
    cr_assert(!is_audio_file(NULL));
    cr_assert(!is_audio_file("noextension"));
}

// ============================================================================
// Metadata Extraction
// ============================================================================

Test(indexer, metadata_extraction_real_files) {
    // Validates metadata extraction from real audio files.
    // PRE_ALL hook initializes FFmpeg before fork to avoid crashes.

    struct {
        const char* path;
        const char* format;
    } test_files[] = {
        { BACH_ALBUM "/01 - Aria.flac", "FLAC" },
        { BEETHOVEN_MP3_ALBUM "/01 - Allegro con brio.mp3", "MP3" },
        { BORODIN_ALBUM "/01 - Moderato-Allegro.ogg", "OGG" },
        { BEETHOVEN_WAV_ALBUM "/01 - Allegro con brio.wav", "WAV" },
    };

    for (size_t i = 0; i < sizeof(test_files)/sizeof(test_files[0]); i++) {
        index_item_t item = {0};
        quadrature_result_t res = extract_audio_metadata(test_files[i].path, &item);

        if (res == QUADRATURE_ERROR_FILE_NOT_FOUND) {
            cr_skip("Test library not downloaded - run tests/assets/download_test_library.sh");
        }

        cr_assert_eq(res, QUADRATURE_OK, "%s extraction failed", test_files[i].format);
        cr_assert_not_null(item.title, "%s should have title", test_files[i].format);
        cr_assert_gt(item.duration_ms, 0, "%s should have duration", test_files[i].format);

        index_item_free(&item);
    }
}

// ============================================================================
// Artwork Discovery
// artwork_find is static in artwork.c — artwork discovery is tested
// end-to-end through the atlas builder pipeline.
// ============================================================================
