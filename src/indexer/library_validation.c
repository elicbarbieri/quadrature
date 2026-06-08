/**
 * library_validation.c - Multi-Disc Album Track Validation
 *
 * Validates track numbering in albums, supporting both:
 *   - Per-disc numbering: Each disc resets to track 1
 *   - Continuous numbering: Track numbers increment globally across discs
 *
 * Examples:
 *   Per-disc:    Disc 1 = [1-12], Disc 2 = [1-10]  (valid)
 *   Continuous:  Disc 1 = [1-12], Disc 2 = [13-22] (valid)
 *   Missing:     Disc 1 = [1, 3-12] (ERROR: missing track 2)
 */

#include "internal.h"
#include <glib.h>
#include <string.h>

/**
 * Detect if an album uses continuous track numbering across discs.
 *
 * Continuous numbering means:
 *   1. Track numbers are globally unique (no duplicates across discs)
 *   2. Track numbers form a continuous sequence 1..N where N = total tracks
 *
 * @param mr Album metadata with extracted tracks
 * @return true if continuous numbering detected, false for per-disc numbering
 */
static bool uses_continuous_numbering(const metadata_result_t* mr) {
    if (mr->track_count == 0) return false;

    /* Single disc albums always use per-disc logic */
    uint16_t max_disc = 0;
    for (size_t i = 0; i < mr->track_count; i++) {
        if (mr->tracks[i].disc_num > max_disc)
            max_disc = mr->tracks[i].disc_num;
    }
    if (max_disc <= 1) return false;

    /* Build a set of all track numbers (ignoring disc_num) */
    bool seen_stack[256];
    bool *seen = seen_stack;
    uint16_t max_track = 0;

    /* Find max track number first */
    for (size_t i = 0; i < mr->track_count; i++) {
        if (mr->tracks[i].track_num > max_track)
            max_track = mr->tracks[i].track_num;
    }

    /* Allocate on heap if too large for stack */
    if (max_track + 1 > 256)
        seen = g_new0(bool, max_track + 1);
    else
        memset(seen, 0, sizeof(bool) * (max_track + 1));

    /* Check for duplicates */
    for (size_t i = 0; i < mr->track_count; i++) {
        uint16_t num = mr->tracks[i].track_num;
        if (num == 0) continue; /* Invalid track number */
        if (num <= max_track && seen[num]) {
            /* Duplicate found - this is per-disc numbering */
            if (seen != seen_stack) g_free(seen);
            return false;
        }
        if (num <= max_track)
            seen[num] = true;
    }

    /* Check if track numbers are contiguous starting from 1.
     * If they form a perfect sequence 1..N, it's continuous numbering.
     * We already checked for duplicates above - if we got here, all track numbers are unique.
     * For multi-disc albums, if tracks don't duplicate and start from 1, assume continuous. */
    bool starts_from_one = false;
    for (size_t i = 0; i < mr->track_count; i++) {
        if (mr->tracks[i].track_num == 1) {
            starts_from_one = true;
            break;
        }
    }
    
    if (!starts_from_one) {
        if (seen != seen_stack) g_free(seen);
        return false;
    }
    
    /* If max track number is significantly larger than track count,
     * and no duplicates exist, this is continuous numbering (possibly with gaps) */
    bool is_continuous = (max_track > mr->track_count) || (max_track == mr->track_count);

    if (seen != seen_stack) g_free(seen);
    return is_continuous;
}

/**
 * Check for gaps in a globally-numbered album (continuous numbering).
 * Reports "Album missing tracks: X, Y-Z" if gaps exist in 1..N sequence.
 */
static void check_global_track_sequence(indexer_t* idx, const metadata_result_t* mr) {
    if (!idx || !mr || !mr->tracks || !mr->dir_path) return;
    if (mr->track_count == 0) return;

    /* Find max track number */
    uint16_t max_track = 0;
    for (size_t i = 0; i < mr->track_count; i++) {
        if (mr->tracks[i].track_num > max_track)
            max_track = mr->tracks[i].track_num;
    }

    if (max_track == 0) return;

    /* Build presence bitset */
    bool present_stack[256];
    bool *present = present_stack;
    if (max_track + 1 > 256)
        present = g_new0(bool, max_track + 1);
    else
        memset(present, 0, sizeof(bool) * (max_track + 1));

    for (size_t i = 0; i < mr->track_count; i++) {
        uint16_t num = mr->tracks[i].track_num;
        if (num > 0 && num <= max_track)
            present[num] = true;
    }

    /* Build human-readable list of missing track numbers */
    GString *missing = g_string_new(NULL);
    for (uint16_t t = 1; t <= max_track; t++) {
        if (present[t]) continue;

        /* Find the end of a contiguous gap range */
        uint16_t range_start = t;
        while (t + 1 <= max_track && !present[t + 1]) t++;

        if (missing->len > 0)
            g_string_append(missing, ", ");
        if (t == range_start)
            g_string_append_printf(missing, "%u", range_start);
        else
            g_string_append_printf(missing, "%u-%u", range_start, t);
    }

    if (missing->len > 0) {
        /* Global numbering - no disc number in error message */
        log_indexer_error(idx, mr->dir_path,
            "Album missing tracks: %s (have %zu of %u)",
            missing->str, mr->track_count, max_track);
    }

    g_string_free(missing, TRUE);
    if (present != present_stack)
        g_free(present);
}

/**
 * Check for gaps in per-disc track numbering.
 * Reports "Album missing tracks (disc N): X, Y-Z" for each disc with gaps.
 */
static void check_per_disc_track_gaps(indexer_t* idx, const metadata_result_t* mr) {
    if (!idx || !mr || !mr->tracks || !mr->dir_path) return;
    if (mr->track_count == 0) return;
    
    /* Find the max disc number to iterate per-disc */
    uint16_t max_disc = 0;
    for (size_t i = 0; i < mr->track_count; i++) {
        if (mr->tracks[i].disc_num > max_disc)
            max_disc = mr->tracks[i].disc_num;
    }

    for (uint16_t disc = 0; disc <= max_disc; disc++) {
        /* Collect track numbers for this disc */
        uint16_t max_track = 0;
        size_t disc_track_count = 0;

        for (size_t i = 0; i < mr->track_count; i++) {
            if (mr->tracks[i].disc_num != disc) continue;
            disc_track_count++;
            if (mr->tracks[i].track_num > max_track)
                max_track = mr->tracks[i].track_num;
        }

        if (disc_track_count == 0 || max_track == 0) continue;

        /* No gaps possible if count equals max */
        if (disc_track_count == max_track) continue;

        /* Build a presence bitset (stack-allocate for typical album sizes) */
        bool present_stack[64];
        bool *present = present_stack;
        if (max_track + 1 > 64)
            present = g_new0(bool, max_track + 1);
        else
            memset(present, 0, sizeof(bool) * (max_track + 1));

        for (size_t i = 0; i < mr->track_count; i++) {
            if (mr->tracks[i].disc_num != disc) continue;
            if (mr->tracks[i].track_num <= max_track)
                present[mr->tracks[i].track_num] = true;
        }

        /* Build a human-readable list of missing track numbers */
        GString *missing = g_string_new(NULL);
        for (uint16_t t = 1; t <= max_track; t++) {
            if (present[t]) continue;

            /* Find the end of a contiguous gap range */
            uint16_t range_start = t;
            while (t + 1 <= max_track && !present[t + 1]) t++;

            if (missing->len > 0)
                g_string_append(missing, ", ");
            if (t == range_start)
                g_string_append_printf(missing, "%u", range_start);
            else
                g_string_append_printf(missing, "%u-%u", range_start, t);
        }

        if (missing->len > 0) {
            if (max_disc > 0) {
                log_indexer_error(idx, mr->dir_path,
                    "Album missing tracks (disc %u): %s (have %zu of %u)",
                    disc, missing->str, disc_track_count, max_track);
            } else {
                log_indexer_error(idx, mr->dir_path,
                    "Album missing tracks: %s (have %zu of %u)",
                    missing->str, disc_track_count, max_track);
            }
        }

        g_string_free(missing, TRUE);
        if (present != present_stack)
            g_free(present);
    }
}

/**
 * Public API: Validate track numbering for an album.
 *
 * Automatically detects continuous vs per-disc numbering and validates
 * accordingly. Logs errors via log_indexer_error() if gaps are found.
 *
 * @param idx Indexer context (for error logging)
 * @param mr Album metadata with extracted tracks
 */
/**
 * Check for duplicate (disc_num, track_num) pairs.
 * Logs an error if multiple tracks have the same disc and track number.
 */
static void check_duplicate_track_positions(indexer_t* idx, const metadata_result_t* mr) {
    if (!idx || !mr || !mr->tracks || !mr->dir_path) return;
    if (mr->track_count < 2) return;

    /* Simple O(n^2) duplicate check on (disc_num, track_num) pairs —
     * fine for typical album sizes. */
    for (size_t i = 0; i < mr->track_count; i++) {
        for (size_t j = i + 1; j < mr->track_count; j++) {
            if (mr->tracks[i].disc_num == mr->tracks[j].disc_num &&
                mr->tracks[i].track_num == mr->tracks[j].track_num) {
                
                const char *path_i = mr->tracks[i].rel_path ? mr->tracks[i].rel_path : "unknown";
                const char *path_j = mr->tracks[j].rel_path ? mr->tracks[j].rel_path : "unknown";
                
                log_indexer_error(idx, mr->dir_path,
                    "Duplicate track position (disc %u, track %u): '%s' and '%s'",
                    mr->tracks[i].disc_num, mr->tracks[i].track_num, path_i, path_j);
                return; /* Report only first duplicate to avoid spam */
            }
        }
    }
}

void validate_album_track_numbering(indexer_t* idx, const metadata_result_t* mr) {
    if (!idx || !mr) return;
    if (mr->track_count == 0) return;
    if (!mr->tracks) return;

    /* Check for duplicate track positions first */
    check_duplicate_track_positions(idx, mr);

    /* Detect numbering pattern and validate accordingly */
    if (uses_continuous_numbering(mr)) {
        check_global_track_sequence(idx, mr);
    } else {
        check_per_disc_track_gaps(idx, mr);
    }
}
