# Indexer Architecture

Eight-phase pipeline. Phases 1–3 populate the library metadata as fast as possible. `INDEXER_LIBRARY_UPDATED` fires after Phase 3 to trigger a library-cache reload so the UI becomes browsable. Phase 4 adds artwork. Phases 5–8 run enrichment (MB resolution, artist art, bios) without blocking browsing.

## Pipeline

```
Phase 1  SCAN          Fast delta detection (stat + mtime hash). Builds work queue.
Phase 2  METADATA      FFmpeg tag extract → per-album transactions → DB write.
Phase 3  FINALIZE      db_set_album_mtimes_batch + WAL checkpoint + orphan error prune.
         ──── INDEXER_LIBRARY_UPDATED ─────────────────────────
           UI: library_cache_refresh_slot() → on_cache_ready → refresh_library_views()
Phase 4  ARTWORK       Atlas generation (parallel, GThreadPool).
         ──── INDEXER_ARTWORK_UPDATED ─────────────────────────
           UI: artwork_manager_reload_library_atlas() → refresh_library_views()
Phase 5  FINGERPRINT   Parallel Chromaprint generation (on-demand, only for untagged albums).
Phase 6  RESOLVE       Batched MusicBrainz Postgres resolution + metadata write.
         ──── INDEXER_LIBRARY_UPDATED ─────────────────────────
           UI: library_cache_refresh_slot() → on_cache_ready → refresh_library_views()
Phase 7  ARTIST_ART    Fetch artist images from fanart.tv → global artist atlas.
         ──── INDEXER_ARTWORK_UPDATED ─────────────────────────
           UI: artwork_manager_reload_artist_atlas() → refresh_library_views()
Phase 8  ARTIST_BIO    Fetch artist bios from Wikipedia via Wikidata → bios DB.
         ──── INDEXER_COMPLETED ────────────────────────────────
```

`INDEXER_LIBRARY_UPDATED` triggers a library-cache reload for the affected slot — old data stays live until the background warming thread finishes, then `on_cache_ready` fires `refresh_library_views()`. `INDEXER_ARTWORK_UPDATED` reloads atlas files and refreshes views directly.

______________________________________________________________________

## Core Invariant: mtime change → full re-process

**If an album directory's mtime or size changes, it is re-processed through the entire pipeline: metadata extraction (Phase 2), artwork generation (Phase 4), and enrichment (Phases 5–8).**

This is the fundamental correctness guarantee. A user replacing `cover.jpg`, fixing tags, adding tracks, or renaming files all manifest as an mtime change on the album directory. The indexer must not skip any phase for changed albums — metadata and artwork are re-derived from the filesystem state, not cached from a previous run.

**Separation of concerns:** The scanner (Phase 1) detects changed directories and pushes them to
a work queue. It knows nothing about atlas state, metadata schema, or enrichment status. Each
downstream phase owns its domain-specific logic:

- Phase 2 (metadata) decides how to extract and write tags from changed directories.
- Phase 4 (artwork) receives the changed directory list AND sweeps its own no-art index to
  promote albums that have since gained fanart covers (downloaded by Phase 7 on a prior run).
  This atlas-specific promotion logic belongs in Phase 4, not the scanner.

______________________________________________________________________

## Phase 1 — SCAN

**Goal:** identify changed albums without reading any audio files.

```
1. db_get_album_mtimes_page() (paged, 1000/page) → GHashTable<abs_path → album_mtime_t>
2. Recurse library root with nftw(FTW_PHYS):
   - stat(dir) → current mtime + entry count + total size
   - lookup in hashmap:
       hit + mtime match + size match → skip (enqueue as unchanged)
       hit + mtime OR size differs    → queue for Phase 2
       miss                           → queue for Phase 2 (new album)
   - remove matched entry from hashmap (mark as "seen")
3. Detect disc subdirectories (CD1/, Disc 1/, etc.) → work_queue_push_multi_disc()
4. Orphan detection: entries remaining in hashmap after walk = albums deleted from disk
   → db_prune_orphan_albums() deletes albums, tracks, track_artists (CASCADE), FTS entries
```

No FFmpeg. No SQLite writes (except orphan pruning). Completes in < 1 s for unchanged libraries.

**Two-factor delta detection:** mtime is the fast path, but unreliable on NFS/SMB (clock
skew), FAT32 (2-second granularity), and `rsync --archive` (preserved mtime). The scan
also tracks file count + total byte size per album directory as a secondary signal. If mtime
matches but count/size differs, the album is re-queued. Cost: one extra `int64` column
(`albums.last_updated_size`), negligible scan overhead (already calling `stat()` per entry).

**Symlink policy:** `nftw()` with `FTW_PHYS` — symlinks are **not followed**. This prevents
infinite loops from symlink cycles and avoids crossing device boundaries. If users need
symlinked album directories, the symlink target should be added as a separate library root.

**DB reads:** `albums.path`, `albums.last_updated_at`, `albums.last_updated_size`, `albums.mb_status`

______________________________________________________________________

## Phase 2 — METADATA

**Goal:** extract file tags and write all track/album/artist data to SQLite. No network. No audio decoding. No fingerprinting.

### Producer-Consumer Write Architecture

FFmpeg extraction runs in parallel via `GThreadPool`, but SQLite is single-writer. To
avoid lock contention, Phase 2 uses a producer-consumer pattern:

```
GThreadPool workers (producers):     Single writer thread (consumer):
  FFmpeg extract tags + duration       Dequeue batch (up to 50 albums)
  Build folder_album_context           BEGIN IMMEDIATE
  Enqueue result to GAsyncQueue   →      for each album in batch:
                                           upsert artists, albums, tracks
                                           db_sync_album_fts(album_id)
                                         COMMIT
                                       Repeat until queue drained + workers done
```

This eliminates `BEGIN IMMEDIATE` contention between workers and enables multi-album
transactions (50 albums per commit vs 1), reducing WAL flush overhead.

The work queue is bounded (`PHASE2_QUEUE_DEPTH`, default 256 items). When full,
`GThreadPool` workers block on enqueue — this provides natural backpressure so large
libraries don't accumulate unbounded metadata in memory.

`get_or_create_artist_cached()` uses an in-memory GHashTable pre-loaded from `SELECT id, name FROM artists` before the GThreadPool starts. Cache misses fall through to `db_get_or_create_artist()` and backfill the cache. Eliminates redundant SELECTs for the common case (same artist across many albums).

### Metadata extraction table

| Tag / Source           | Extract logic                                                                       | SQLite column written                                                |
| ---------------------- | ----------------------------------------------------------------------------------- | -------------------------------------------------------------------- |
| `ARTIST`               | `parse_artist_tag()` — split on `feat.` / `ft.` / `&`                               | `artists.name`, `track_artists.*`, `tracks.artist_display`           |
| `ALBUMARTIST`          | Prefer over ARTIST for album-level artist                                           | `albums.artist_id` → `artists.name`                                  |
| `ALBUM`                | Folder name takes precedence over tag                                               | `albums.title`                                                       |
| `TITLE`                | Raw tag value                                                                       | `tracks.title`                                                       |
| `TRACKNUMBER`          | Parsed integer (strips `x/y` format)                                                | `tracks.track_num`                                                   |
| `DISCNUMBER`           | Parsed integer; disc folder name overrides (CD1 → 1)                                | `tracks.disc_num`                                                    |
| `DATE` / `YEAR`        | First 4-digit run                                                                   | `tracks.year`, `albums.year`                                         |
| `GENRE`                | Raw tag value                                                                       | `tracks.genre`                                                       |
| `MUSICBRAINZ_ALBUMID`  | Stored verbatim; used by Phase 6 for direct Postgres lookup (no fingerprint needed) | `albums.musicbrainz_release_id`, `albums.mb_status = HAS_RELEASE_ID` |
| FFmpeg stream duration | `stream->duration * time_base`                                                      | `tracks.duration_ms`                                                 |
| `stat().st_mtime`      | File modification time                                                              | held in memory → flushed in Phase 3 as `albums.last_updated_at`      |
| file path              | Relative to album directory                                                         | `tracks.path`                                                        |
| album directory        | Relative to library root                                                            | `albums.path`                                                        |

**Not extracted in Phase 2:** MB release group, sort names. Those come from Phase 6. Release-level metadata (label, catalog number, barcode, release type) is stored in the metadata DB, not the main DB.

### FTS writes

`tracks_fts(rowid, title, artist, album)` is written **once per album** after all tracks and artist credits are committed (`db_sync_album_fts`). Per-track FTS writes are eliminated — the bulk sync reads `artist_display` which is already set by `db_set_track_artists`.

FTS5 tables are **standalone** (not `content-sync`) because the FTS columns are derived from JOINs (`artist_display` from track_artists, album title from albums table), not direct column mappings. Manual sync via `db_sync_album_fts()` is the correct approach for this schema.

### Performance design

| Technique                         | Benefit                                                           |
| --------------------------------- | ----------------------------------------------------------------- |
| Producer-consumer + batched txns  | 50 albums per `BEGIN IMMEDIATE`; zero lock contention             |
| Bounded work queue (256 items)    | Backpressure prevents unbounded memory growth on large libraries  |
| In-memory artist name cache       | Eliminates repeated `SELECT id FROM artists WHERE name=?`         |
| 8 pre-compiled SQLite statements  | No per-call `sqlite3_prepare_v2` on hot paths                     |
| Bulk album FTS sync               | N per-track FTS writes → 1 per album                              |
| `PRAGMA wal_autocheckpoint=10000` | Defers checkpoint until ~40 MB burst; keeps WAL writes sequential |
| Periodic `PASSIVE` checkpoint     | Every 500 albums; prevents WAL from growing unbounded during bulk |

______________________________________________________________________

## Phase 3 — FINALIZE (Metadata)

**Goal:** flush mtime data, prune orphans, and checkpoint WAL so the library is immediately usable.

```
db_set_album_mtimes_batch()      ← bulk UPDATE albums SET last_updated_at=?
db_prune_orphan_artists()        ← remove artists with no remaining track/album refs
db_prune_orphan_errors()         ← remove errors for directories that no longer exist
db_checkpoint(PASSIVE)           ← WAL flush
→ INDEXER_LIBRARY_UPDATED fires
```

**Note:** Orphan *album* pruning happens in Phase 1 (scan), not Phase 3. Phase 1 detects deleted directories via mark-and-sweep on the album_mtimes hash table and calls `db_prune_orphan_albums()` immediately, which cascades to tracks → track_artists → FTS cleanup. Phase 3's `db_prune_orphan_artists()` then cleans up any artists left without references.

Running the mtime batch before the library-updated signal ensures Phase 1 correctly skips unchanged albums on the next run, even if the process is killed before Phase 6 completes.

______________________________________________________________________

## Phase 4 — ARTWORK

**Goal:** write a timestamped atlas file. Pure image processing, no DB metadata writes.

**Two input sources:**

1. **Changed albums** from Phase 2 (`processed_album_t[]`) — directories the scanner detected as
   modified. These are re-scanned for artwork unconditionally (core invariant: mtime change → full
   re-process).
2. **No-art promotion** (atlas-internal) — `artwork_atlas_builder_sweep_no_art()` walks the atlas
   no-art list and checks if a fanart.tv cover has since been downloaded (by Phase 7 on a prior
   run). Promoted album IDs are returned and queued for artwork extraction. This is the only
   recovery path — it handles the case where an album had no local artwork, then got a MusicBrainz
   ID (Phase 6), and a fanart cover was downloaded (Phase 7). No full DB scan needed.

```
1. Open previous atlas (if exists) → read existing album_id index
2. Sweep no-art list: promote albums that now have fanart covers → queue promoted IDs
3. GThreadPool workers per album (changed + promoted):
   - scan for cover.jpg / folder.jpg / front.jpg / embedded art
   - fanart.tv cover fallback (via release_group_id)
   - resize to thumb_size pixels (default 48)
   - add RGB blob to atlas builder
4. Preserve unchanged entries from prior atlas (O(log n) binary search)
5. artwork_atlas_builder_finish() → write atlas atomically
6. Rotate: keep 3 most recent atlas files
```

**DB reads only.** Writes zero SQLite rows.

______________________________________________________________________

## Phase 5 — FINGERPRINT

**Goal:** generate Chromaprint fingerprints for albums that need acoustic identification.
Skipped if `mb_resolve = false` or `pg_conninfo` is empty.

- Decode first 30 seconds (`MB_FINGERPRINT_DURATION`) of up to 4 tracks (`MB_FINGERPRINT_TRACKS`) per album
- Mono 11025Hz via libchromaprint
- Fingerprints held **in memory** on the resolver context, passed to Phase 6
- NOT stored in the database

Albums with `mb_status = HAS_RELEASE_ID` (Phase 2 found a `MUSICBRAINZ_ALBUMID` tag) skip fingerprinting entirely.

______________________________________________________________________

## Phase 6 — RESOLVE (MusicBrainz)

**Goal:** enrich albums with full MB metadata. Skipped if `mb_resolve = false` or `pg_conninfo` is empty.

### Resolution strategy (per album)

```
Step 1: check albums.musicbrainz_release_id
        → if set (from Phase 2 tag extraction): use directly → skip to Step 3

Step 2: find_release_by_fingerprint()   [only for untagged albums]
        Fallback chain:
        a) ISRC lookup: batch query ISRCs from file tags against MB PG
        b) Solr text search: album title + artist name → duration validation
           - Folder-path fallback: if file tags have no album/artist, extract
             from album_path (grandparent = artist, parent = album). Strips
             year suffixes like "(2020)". Same heuristic as Picard's
             album_artist_from_path().
           - Edition stripping: parenthetical edition text like
             "(10th Anniversary Edition)" is stripped from the SOLR query to
             improve recall. Post-scoring via similarity2() uses the original
             title for accurate matching.
        c) AcoustID fingerprint: query local AcoustID PG via acoustid_compare2()
           → consensus vote (best_count / fingerprinted >= 80%)

Step 3: mb_fetch_all_batch(pg_client, release_ids[], count)
        → single Postgres call fetches up to 50 releases per round-trip
        → returns releases + recordings + credits + artist-recording links

Step 4: match_tracks(release, local_tracks)
        → Pass 1: exact (disc_num, position)
        → Pass 2: score = duration_sim×0.6 + title_jaccard×0.4; threshold 0.5

Step 5: db_begin_transaction()
          db_update_album_mb()                ← album metadata
          db_update_album_artist() via db_get_or_create_artist_mb()
          for each matched track:
            db_update_track_title()
            db_set_track_artists() via db_get_or_create_artist_mb()
          db_sync_album_fts(album_id)         ← one bulk FTS sync
        db_commit()                           ← durable; visible to LibraryCache on next warm
```

Each album commit is independent. A kill mid-Phase 6 leaves resolved albums in the DB and unresolved albums with their Phase 2 data intact — fully browseable. The next run resumes from `mb_status IN (NOT_ATTEMPTED, HAS_RELEASE_ID)`.

### PostgreSQL connection resilience

Phase 6 must tolerate flaky PG connections (especially over VPN or WAN):

- **Per-batch retry:** if `mb_fetch_all_batch()` fails, retry once after reconnect. If retry
  fails, mark affected albums as `FAILED` and continue with remaining batches.
- **Circuit breaker:** after 3 consecutive PG failures, skip remaining Phase 6 work and
  proceed to Phase 7. Avoids blocking the entire enrichment pipeline on a dead database.
- **Partial-phase resume:** already handled by `mb_status` — next run picks up `FAILED` albums
  only if explicitly reset to `NOT_ATTEMPTED`.

After all albums are processed, `db_prune_orphan_artists()` removes Phase 2 artists that were replaced by corrected MusicBrainz entries.

### Phase 6 metadata table

| MB field                       | Source                        | Processing                           | Written to                                                              |
| ------------------------------ | ----------------------------- | ------------------------------------ | ----------------------------------------------------------------------- |
| Release title                  | Postgres `release.name`       | Overwrites Phase 2 folder name       | `albums.title` (main DB)                                                |
| `musicbrainz_release_id`       | Tags or AcoustID consensus    | —                                    | `albums.musicbrainz_release_id` (main DB)                               |
| `musicbrainz_release_group_id` | Postgres                      | —                                    | `albums.musicbrainz_release_group_id` (main DB)                         |
| Release date                   | Postgres                      | Extract 4-digit year                 | `albums.year` (main DB, only if > 0)                                    |
| `mb_status`                    | Set to `MB_STATUS_RESOLVED`   | —                                    | `albums.mb_status`, `albums.mb_resolved_at` (main DB)                   |
| Artist name                    | Postgres `artist_credit.name` | `db_get_or_create_artist_mb()`       | `artists.name`, `artists.musicbrainz_id`, `artists.sort_name` (main DB) |
| Track artist credits           | Postgres per recording        | Replaces Phase 2 credits             | `track_artists.*`, `tracks.artist_display` (main DB)                    |
| Track title                    | Postgres recording title      | Overwrites Phase 2 title             | `tracks.title` (main DB)                                                |
| `release_type`                 | Postgres                      | Album / Single / EP / etc.           | `releases.release_type` (metadata DB)                                   |
| `label`                        | Postgres                      | First release label                  | `releases.label` (metadata DB)                                          |
| `catalog_number`               | Postgres                      | —                                    | `releases.catalog_number` (metadata DB)                                 |
| `barcode`                      | Postgres                      | —                                    | `releases.barcode` (metadata DB)                                        |
| `genres`                       | Postgres                      | Comma-separated                      | `releases.genres` (metadata DB)                                         |
| Recording links                | Postgres `l_artist_recording` | Producers, remixers, vocalists, etc. | `recording_links` (metadata DB)                                         |
| AcoustID fingerprint           | Generated on-demand           | 30s Chromaprint decode               | Never persisted to DB                                                   |

______________________________________________________________________

## Phase 7 — ARTIST_ART (fanart.tv)

**Goal:** fetch artist thumbnail images for all MusicBrainz-resolved artists.

- Queries fanart.tv API for artist images (rate-limited at 500ms between requests)
- Downloads thumbnails, resizes to `thumb_size` pixels via libvips
- Builds/updates the **global** artist atlas at `~/.local/share/quadrature/atlas/artists.atlas`
- Atlas is UUID-keyed (16-byte binary MusicBrainz UUIDs), sorted for binary search
- Uses `flock()` on `artists.atlas.lock` for write serialization across concurrent indexer runs
- Scans other libraries' artwork directories to avoid re-downloading already-cached images
- Tracks artists with no available artwork (no-art UUIDs) to skip them on future runs

**Multi-library deduplication:** each library run loads the existing global atlas, scans its
own MBID set, copies already-downloaded art from other libraries' `artwork/` dirs
(`copy_art_from_other_library`), fetches only the remainder from fanart.tv, and writes
a merged atlas. Net effect: library N only downloads art for artists not already covered
by libraries 1…N-1. The atlas and all UI lookups are MBID-keyed and library-mask-agnostic —
art is available regardless of which library filter is active.

______________________________________________________________________

## Phase 8 — ARTIST_BIO (Wikipedia)

**Goal:** fetch artist biographies from Wikipedia for all MusicBrainz-resolved artists.

- Looks up Wikidata entity via artist MBID → fetches English Wikipedia summary
- Writes to `{data_root}/quadrature-bios.sqlite` — separate from metadata DB so deleting
  the metadata DB (to force MB re-resolve) doesn't destroy expensive-to-refetch bios
- Rate-limited at 250ms between requests
- Auto-migrates existing bios from `quadrature-metadata.sqlite` on first open (one-time)

Bios are stored per-library (each library's `quadrature-bios.sqlite`), but the UI queries
all libraries' bio DBs by MBID regardless of the active library filter — same
library-mask-agnostic principle as artist art.

After Phase 8 completes: `INDEXER_COMPLETED` fires.

______________________________________________________________________

## External API Resilience (Phases 7–8)

Both phases hit rate-limited external services. Error handling strategy:

- **Exponential backoff** on HTTP 429/5xx: start 1s, max 30s, 3 retries per artist
- **Poison tracking:** artists that fail 3 times across runs are recorded in a
  `failed_fetches` set (in-memory, per-run) and skipped for the remainder of the phase.
  The no-art UUID list in the artist atlas serves this role for Phase 7.
- **Phase timeout:** configurable max wall-clock per phase (default 30 min). If exceeded,
  the phase terminates gracefully — completed work is preserved, remaining artists are
  picked up on the next run.
- **DNS/network failure:** treated as transient. After 3 consecutive network errors
  (not per-artist — consecutive), the phase aborts early.

______________________________________________________________________

## Indexer Telemetry

Each phase records wall-clock timing and per-phase counters for operational visibility.
Exposed via `indexer_get_stats()` after completion and emitted in the `INDEXER_COMPLETED`
signal payload.

| Phase | Metrics |
| ----- | ------- |
| 1 — SCAN | `dirs_scanned`, `dirs_queued`, `dirs_unchanged`, `scan_duration_ms` |
| 2 — METADATA | `albums_processed`, `tracks_extracted`, `ffmpeg_failures`, `albums_per_sec`, `metadata_duration_ms` |
| 3 — FINALIZE | `mtime_updates`, `errors_pruned`, `finalize_duration_ms` |
| 4 — ARTWORK | `atlases_built`, `entries_preserved`, `entries_new`, `artwork_duration_ms` |
| 5 — FINGERPRINT | `albums_fingerprinted`, `tracks_fingerprinted`, `fingerprint_duration_ms` |
| 6 — RESOLVE | `albums_attempted`, `albums_resolved`, `albums_no_match`, `albums_failed`, `pg_query_avg_ms`, `resolve_duration_ms` |
| 7 — ARTIST_ART | `artists_fetched`, `artists_skipped`, `http_errors`, `artist_art_duration_ms` |
| 8 — ARTIST_BIO | `artists_fetched`, `artists_skipped`, `http_errors`, `artist_bio_duration_ms` |

______________________________________________________________________

## Durability & Crash Recovery

| Kill point                     | State on restart                                                                                                                                |
| ------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------- |
| During Phase 2                 | Partially-written album has no `last_updated_at` → Phase 1 re-queues it. No stale data visible.                                                 |
| After Phase 3 (before Phase 4) | Full library browseable with Phase 2 data. Artwork may be stale (old atlas used).                                                               |
| During Phase 4                 | Atlas may be incomplete but old atlas still exists → prior atlas used. Phase 2 data fully in DB.                                                |
| After Phase 4 (before Phase 5) | Full library browseable with artwork. Phase 6 restarts from all unresolved albums (`mb_status != RESOLVED`).                                    |
| Mid-Phase 6                    | Resolved albums durable (each has its own commit). Unresolved albums show Phase 2 data. Next run resumes from remaining unresolved albums only. |
| During Phase 7/8               | Artist art/bios partially complete. Existing atlas/bios intact. Next run picks up where it left off (no-art/already-fetched tracking).          |

______________________________________________________________________

## Thread Model

```
Main thread (GTK)            Indexer worker thread         GThreadPool workers
═════════════════            ══════════════════════        ═══════════════════
indexer_scan() ──────────→  indexer_worker()
                             phase_scan()
                             phase_metadata()  ─────────→  metadata_worker() × N
                               [waits for pool]  ←────────   (per album)
                             phase_finalize()
                             g_idle_add(LIBRARY_UPDATED) → on_indexer_library_updated()
                                                              → library_cache_refresh_slot()
                             phase_artwork()   ─────────→  artwork_worker() × N
                               [waits for pool]  ←────────
                             g_idle_add(ARTWORK_UPDATED) → on_indexer_artwork_updated()
                                                              → atlas reload
                                                              → refresh views
                             mb_resolver_run()              (fingerprint + resolve)
                             g_idle_add(LIBRARY_UPDATED) → on_indexer_library_updated()
                                                              → library_cache_refresh_slot()
                             artist_art_fetch_all()         (fanart.tv HTTP)
                             g_idle_add(ARTWORK_UPDATED) → on_indexer_artwork_updated()
                                                              → artist atlas reload
                             artist_bio_fetch_all()         (Wikipedia HTTP)
                             g_idle_add(COMPLETED) ──────→ on_indexer_done()
```

- LibraryCache `db_ui` + `db_warm` are **read-only** SQLite connections. SQLite WAL allows unlimited concurrent readers alongside a single writer — Phase 6 writes never block cache reads.
- Phase 6 commits atomically per album. Each commit is immediately visible to `db_ui` / `db_warm` readers via WAL read-through.
- If the user kills the app during Phase 6, the next launch re-warms the cache from the DB; resolved albums are fully visible, unresolved show Phase 2 data.

______________________________________________________________________

## mb_status State Machine

```
NOT_ATTEMPTED (0)   ──Phase 2──→  HAS_RELEASE_ID (1)  [if MUSICBRAINZ_ALBUMID tag present]
NOT_ATTEMPTED (0)   ──Phase 6──→  RESOLVED (2) | NO_MATCH (3) | FAILED (4)
HAS_RELEASE_ID (1)  ──Phase 6──→  RESOLVED (2) | FAILED (4)
```

| Value | Constant         | Meaning                                                      |
| ----- | ---------------- | ------------------------------------------------------------ |
| `0`   | `NOT_ATTEMPTED`  | No MB work done; no release ID in DB                         |
| `1`   | `HAS_RELEASE_ID` | Release UUID found in Picard tags (Phase 2); not yet fetched |
| `2`   | `RESOLVED`       | Fully resolved: MB PG data fetched and written to SQLite     |
| `3`   | `NO_MATCH`       | Resolution attempted but no confident match found            |
| `4`   | `FAILED`         | Resolution attempted but errored                             |

Phase 6 queries `WHERE mb_status IN (0, 1)`. Albums with status 2/3/4 are skipped.
