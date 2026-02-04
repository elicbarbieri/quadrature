# CLAUDE.md

Guidance for Claude Code when working with this codebase.

## Build

```bash
nix develop     # Enter dev environment
make debug      # Build and run with DEBUG logging
make test       # Run tests
make clean      # Clean build
```

## Directory Structure

```
include/quadrature/
  audio/     # audio_pipeline.h, audio_ringbuf.h, audio_spectrum.h
  core/      # engine.h, config.h, logging.h, types.h
  library/   # library.h, library_index.h, library_sync.h
  indexer/   # indexer.h, indexer_core.h, indexer_pipeline.h

src/
  audio/     # Pipeline, ring buffer, spectrum analyzer
  core/      # Engine, config, logging
  library/   # SQLite music library (v4 schema with album hashing)
  indexer/   # Four-phase queue-based indexer
  ui/        # GTK4 widgets (.c/.h source files)
    templates/  # .ui templates, .css stylesheets, gresource.xml

bin/             # Entry points (quadrature_indexer.c)
tests/unit/      # Criterion tests
third_party/     # cavacore (spectrum FFT)
```

## Code Conventions

- Functions/variables: `snake_case`
- Constants/macros: `UPPER_CASE`
- Types: suffix `_t` (e.g., `audio_pipeline_t`)
- All public functions return `quadrature_result_t`
- Logging: LOG_ERROR_MSG/LOG_WARN_MSG/LOG_INFO_MSG/LOG_DEBUG_MSG

## Key Patterns

### Create/Destroy

```c
audio_pipeline_t* pipeline = NULL;
quadrature_result_t res = audio_pipeline_create(48000, &pipeline);
if (res != QUADRATURE_OK) { /* handle error */ }
// ...
audio_pipeline_destroy(pipeline);
```

### Audio Thread Safety

- Audio callbacks must be lock-free (no malloc, no mutex)
- Use atomics for shared state between UI and audio threads
- Pre-allocate all buffers before audio starts
- Use lock-free ring buffers for cross-thread data (audio_ringbuf.c)

### Error Handling

```c
quadrature_result_t res = some_function();
if (res != QUADRATURE_OK) {
    LOG_ERROR_MSG("Operation failed: %d", res);
    return res;
}
```

### Defensive Programming (NASA-Style)

**Crash on invariant violations. No silent fallbacks.**

- Use `g_assert()` or `g_error()` to enforce design invariants
- Never silently ignore invalid state - crash immediately with clear error
- Remove redundant fallback logic that masks bugs
- If a precondition is violated, crash with a descriptive message
- Prefer "fail fast" over "fail safe" during development

```c
// GOOD: Crash on invariant violation
void audio_cache_lock(audio_cache_t* cache, int64_t track_id) {
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);
    if (!buffer) {
        g_error("audio_cache_lock: track %" G_GINT64_FORMAT " not loaded - "
                "call audio_cache_load() first", track_id);
    }
    atomic_fetch_add(&buffer->lock_count, 1);
}

// BAD: Silent fallback that hides bugs
void audio_cache_lock(audio_cache_t* cache, int64_t track_id) {
    audio_buffer_t* buffer = g_hash_table_lookup(cache->buffers, &track_id);
    if (buffer) {  // Silently does nothing if not found!
        atomic_fetch_add(&buffer->lock_count, 1);
    }
}
```

**API contracts must be explicit:**
- Document preconditions in comments
- Enforce preconditions with asserts
- Caller is responsible for meeting preconditions

### Indexer Architecture

Four-phase queue-based design (~100-200 lines core logic):

```
PHASE 1 - SCAN (fast, single-threaded):
  1. db_get_album_mtimes_page() → build GHashTable of path → (album_id, last_updated_at)
  2. Walk directories recursively:
     - stat(dir) to get current mtime
     - Lookup in hashmap
     - If mtime matches: skip (unchanged)
     - If mtime differs or missing: queue (dir_path, files) for processing
  Should complete in <1 second for unchanged libraries.

PHASE 2 - METADATA (parallel, GThreadPool):
  Process queued directories:
  - Extract metadata from audio files (FFmpeg)
  - Build folder_album_context
  - Write tracks/albums to DB
  - Track which albums were successfully processed

PHASE 3 - ARTWORK (parallel, GThreadPool):
  For each album in work queue:
  - Extract/resize album art
  - Write to atlas
  Pure image processing - no DB mtime logic.

PHASE 4 - FINALIZE (single-threaded):
  - db_set_album_mtimes_batch() for all successfully processed albums
  - Update error flags for albums with issues
  - WAL checkpoint
```

**Key DB APIs:**
- `db_get_album_mtimes_page(db, offset, limit, &out, &count)` → paged fetch of `{album_id, path, last_updated_at}`
- `db_set_album_mtimes_batch()` → bulk update mtimes in finalize phase
- No separate dir_mtime table - album table stores path + mtime

**Key principles:**
- Scan phase is FAST: just stat() and hashmap lookups, no FFmpeg
- Heavy work queued during scan, processed in parallel AFTER
- Delta detection via album.last_updated_at (not separate table)
- Artwork phase is pure image processing (no DB bookkeeping)
- All mtime/error updates consolidated in finalize phase
- Progress callbacks throttled to 100ms (per-instance, not global)

**Thread safety:**
- Worker threads for metadata extraction via GThreadPool
- Callbacks invoked from indexer thread (not worker threads)
- Safe cancellation via atomic flag checked between operations
- Progress state protected by mutex for current_path and phase_start_time

## Version Control (jj)

```bash
jj status              # Show status
jj log                 # Show history
jj diff                # Show changes
jj new                 # Create new commit
jj describe -m "msg"   # Set commit message
jj squash              # Squash into parent
```
