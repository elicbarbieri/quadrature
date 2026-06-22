# CLAUDE.md

Guidance for Claude Code when working with this codebase.

## ⚠ Minimal Storage Principle — CRITICAL

**Store the minimum data in SQLite. Derive everything else on demand.**

- Every new DB column must justify itself: what query requires it that cannot be answered from existing data?
- No redundant identifiers: `tracks` has no `musicbrainz_recording_id` — tracks are addressed by `(disc_num, track_num)` within `albums.musicbrainz_release_id`.
- No convenience caches: if a value can be computed at query time in < 1ms, do not persist it.
- No "just in case" columns. Schema debt compounds on every re-index and migration.
- Full rules: `docs/architecture/LIBRARY_SYSTEM.md` → "Minimal Storage Principle"

## Build

```bash
nix develop     # Enter dev environment
make debug      # Build and run with DEBUG logging
make test       # Run tests
make clean      # Clean build
```

## Directory Structure

```
include/quadrature/       # Flat public headers (no subdirectories)
  quadrature.h            # Core types (quadrature_result_t, etc.)
  audio.h                 # Audio pipeline, ring buffer, spectrum
  database.h              # SQLite database API
  indexer.h               # Indexer + MusicBrainz resolver API
  library.h               # Library cache + global ID macros
  metadata.h              # Metadata types
  perf.h                  # Performance system monitor
  settings.h              # App settings (settings.ini)

src/
  audio/                  # Pipeline, ring buffer, spectrum analyzer
  core/                   # Perf system monitor + shared internal types
  database/               # SQLite music library (read/write, reconciler, migrations)
  library/                # Warm in-memory library cache + search
  controller/             # Channel control sources (generic API + Axia/Livewire backend)
  cli/                    # quadrature-cli unified entry point (main.c) + subcommands
  indexer/                # Queue-based indexer (scan → metadata → finalize → artwork + enrichment)
    musicbrainz/          # MusicBrainz/AcoustID resolution
  ui/                     # GTK4 widgets (.c/.h source files)
    library/              # Library browser widgets
    perf/                 # Performance dashboard widgets
    templates/            # .ui templates, .css stylesheets, gresource.xml

tests/unit/               # Criterion tests
```

**Internal header rule:** Each `src/` subdirectory has at most one `internal.h` for private declarations. No other `.h` files in `src/`. The one deliberate exception is `src/audio/cavacore.h` — vendored third-party (MIT) kept separate to ease any future re-syncing with upstream.

## Code Conventions

- Functions/variables: `snake_case`
- Constants/macros: `UPPER_CASE`
- Types: suffix `_t` (e.g., `audio_pipeline_t`)
- All public functions return `quadrature_result_t`
- Logging: GLib functions directly — `g_message` (info), `g_warning` (recoverable), `g_critical` (errors / data loss), `g_debug`. Each `.c` sets `#define G_LOG_DOMAIN "quadrature"` at the top.

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
    g_warning("Operation failed: %d", res);
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

### Library Cache Pointer Safety

`library_cache_get_*` functions return **interior pointers** into the cache's slot arrays. These are valid only while the cache is warm. `library_cache_clear()` frees all slot arrays — any stored raw pointer becomes dangling.

**Rule: never store a raw cache pointer in widget data that outlives the row-creation call.**

```c
// BAD — dangling after library_cache_clear()
abd->track_artists = library_cache_get_track_artists(cache, track_id);

// GOOD — re-fetch on every access; returns NULL gracefully after clear
abd->cache    = cache;
abd->track_id = track_id;
// then: library_cache_get_track_artists(abd->cache, abd->track_id)
```

Within a single row-creation function, cache interior pointers are safe to use for immediate widget setup (setting labels, requesting artwork). They must not be stored in tick-callback structs, GObject data keys, or any other structure that persists across frames.

See `docs/architecture/LIBRARY_CACHE.md` → "Pointer Lifetimes & UI Safety" for the full picture.

### Indexer Architecture

Queue-based design. The worker (`indexer_worker`) runs the core phases in order, then
optional enrichment phases. Each phase is a `phase_*(idx)` helper:

```
SCAN (fast, single-threaded):
  1. db_get_album_mtimes_page() → build GHashTable of path → (album_id, last_updated_at)
  2. Walk directories recursively:
     - stat(dir) to get current mtime
     - Lookup in hashmap
     - If mtime matches: skip (unchanged)
     - If mtime differs or missing: queue (dir_path, files) for processing
  Should complete in <1 second for unchanged libraries.

METADATA (parallel, GThreadPool):
  Process queued directories:
  - Extract metadata from audio files (FFmpeg)
  - Build folder_album_context
  - Write tracks/albums to DB (dedicated writer thread, batched transactions)
  - Track which albums were successfully processed

FINALIZE (single-threaded — runs BEFORE artwork, for durability):
  - db_set_album_mtimes_batch() for all successfully processed albums
  - Prune orphan artists/errors
  - WAL checkpoint
  → emits LIBRARY_UPDATED: metadata usable, UI browsable

ARTWORK (parallel, GThreadPool):
  Extract/resize album art, write to atlas. Pure image processing, no DB mtime logic.

Enrichment (optional, gated by settings; each a no-op when disabled):
  - RESOLVE       — Chromaprint fingerprint + MusicBrainz/AcoustID resolution
  - ARTIST_ART    — fetch artist images from fanart.tv + album covers
  - ARTIST_BIO    — fetch artist bios from Wikipedia
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
