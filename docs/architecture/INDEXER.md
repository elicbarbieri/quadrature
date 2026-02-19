# Indexer Architecture

Five-phase pipeline. Phases 1–3 populate the library as fast as possible. `INDEXER_LIBRARY_READY` fires after Phase 3 so the UI becomes usable immediately. Phases 4–5 run on the same background thread and complete MB enrichment without blocking the UI.

## Pipeline

```
Phase 1  SCAN        Fast delta detection (stat + mtime hash). Builds work queue.
Phase 2  METADATA    FFmpeg tag extract → per-album transactions → DB write.
Phase 3  ARTWORK     Atlas generation (parallel, GThreadPool).
         ──── db_set_album_mtimes_batch + WAL checkpoint ────
         ──── INDEXER_LIBRARY_READY ────────────────────────
           UI: warm library cache → on_cache_ready → refresh_library_views()
         ──── INDEXER_ARTWORK_READY ────────────────────────
           UI: reload atlas → refresh_library_views()
Phase 4  RESOLVE     Fingerprint on-demand + MusicBrainz Postgres resolution.
Phase 5  FINALIZE    Final WAL checkpoint.
         ──── INDEXER_COMPLETED ─────────────────────────────
           UI: re-warm library cache → on_cache_ready → refresh_library_views()
```

`INDEXER_LIBRARY_READY` carries `atlas_path`. All three signals (LIBRARY_READY, ARTWORK_READY, COMPLETED) ultimately call `refresh_library_views()` — either deferred via `on_cache_ready` after warming, or directly after atlas reload.

---

## Phase 1 — SCAN

**Goal:** identify changed albums without reading any audio files.

```
1. db_get_album_mtimes_page() (paged, 1000/page) → GHashTable<abs_path → album_mtime_t>
2. Recurse library root with opendir/readdir:
   - stat(dir) → current mtime
   - lookup in hashmap:
       hit + mtime match  → skip (enqueue as unchanged)
       hit + mtime differs → queue for Phase 2
       miss               → queue for Phase 2 (new album)
3. Detect disc subdirectories (CD1/, Disc 1/, etc.) → work_queue_push_multi_disc()
```

No FFmpeg. No SQLite writes. Completes in < 1 s for unchanged libraries.

**DB reads:** `albums.path`, `albums.last_updated_at`, `albums.mb_status`

---

## Phase 2 — METADATA

**Goal:** extract file tags and write all track/album/artist data to SQLite. No network. No audio decoding.

### Per-album transaction scope

All DB writes for one album execute inside a single `BEGIN IMMEDIATE` / `COMMIT`:

```
db_begin_transaction()
  get_or_create_artist_cached()       ← album artist (in-memory cache first)
  db_upsert_folder_album()            ← INSERT OR UPDATE albums
  db_set_album_release_id_from_tags() ← stores MUSICBRAINZ_ALBUMID if present
  for each track:
    db_upsert_track_with_album()      ← INSERT OR CONFLICT UPDATE tracks
    write_phase2_track_artists()      ← get_or_create_artist_cached per credit
                                         db_set_track_artists()
  db_sync_album_fts(album_id)         ← one bulk FTS sync for all tracks
db_commit()
```

`get_or_create_artist_cached()` uses an in-memory GHashTable pre-loaded from `SELECT id, name FROM artists` before the GThreadPool starts. Cache misses fall through to `db_get_or_create_artist()` and backfill the cache. Eliminates redundant SELECTs for the common case (same artist across many albums).

### Metadata extraction table

| Tag / Source | Extract logic | SQLite column written |
|---|---|---|
| `ARTIST` | `parse_artist_tag()` — split on ` feat. ` / ` ft. ` / ` & ` | `artists.name`, `track_artists.*`, `tracks.artist_display` |
| `ALBUMARTIST` | Prefer over ARTIST for album-level artist | `albums.artist_id` → `artists.name` |
| `ALBUM` | Folder name takes precedence over tag | `albums.title` |
| `TITLE` | Raw tag value | `tracks.title` |
| `TRACKNUMBER` | Parsed integer (strips `x/y` format) | `tracks.track_num` |
| `DISCNUMBER` | Parsed integer; disc folder name overrides (CD1 → 1) | `tracks.disc_num` |
| `DATE` / `YEAR` | First 4-digit run | `tracks.year`, `albums.year` |
| `GENRE` | Raw tag value | `tracks.genre` |
| `MUSICBRAINZ_ALBUMID` | Stored verbatim; used by Phase 4 for direct Postgres lookup (no fingerprint needed) | `albums.musicbrainz_release_id`, `albums.mb_status = HAS_RELEASE_ID` |
| FFmpeg stream duration | `stream->duration * time_base` | `tracks.duration_ms` |
| `stat().st_mtime` | File modification time | held in memory → flushed in Phase 3.5 as `albums.last_updated_at` |
| file path | Relative to album directory | `tracks.path` |
| album directory | Relative to library root | `albums.path` |

**Not extracted in Phase 2:** MB release group, label, catalog number, barcode, sort names, credited names. Those come from Phase 4.

### FTS writes

`tracks_fts(rowid, title, artist, album)` is written **once per album** after all tracks and artist credits are committed (`db_sync_album_fts`). Per-track FTS writes are eliminated — the bulk sync reads `artist_display` which is already set by `db_set_track_artists`.

### Performance design

| Technique | Benefit |
|---|---|
| One `BEGIN IMMEDIATE` per album | Eliminates 3 implicit transaction flushes per album |
| In-memory artist name cache | Eliminates repeated `SELECT id FROM artists WHERE name=?` |
| 8 pre-compiled SQLite statements | No per-call `sqlite3_prepare_v2` on hot paths |
| Bulk album FTS sync | N per-track FTS writes → 1 per album |
| `PRAGMA wal_autocheckpoint=10000` | Defers checkpoint until ~40 MB burst; keeps WAL writes sequential |

---

## Phase 3 — ARTWORK

**Goal:** write a timestamped atlas file. Pure image processing, no DB metadata writes.

```
1. Open previous atlas (if exists) → read existing album_id index
2. GThreadPool workers per album:
   - scan for cover.jpg / folder.jpg / front.jpg / embedded art
   - resize to thumb_size pixels (default 48)
   - add PNG blob to atlas builder
3. Preserve unchanged entries from prior atlas (O(log n) binary search)
4. artwork_atlas_builder_finish() → write atlas atomically
5. Rotate: keep 3 most recent atlas files
```

**DB reads only.** Writes zero SQLite rows.

### Phase 3.5 — Metadata Finalize (runs immediately after Phase 3, before LIBRARY_READY)

```
db_set_album_mtimes_batch()   ← bulk UPDATE albums SET last_updated_at=?
db_checkpoint(PASSIVE)        ← WAL flush
→ INDEXER_LIBRARY_READY fires (carries atlas_path)
```

Running the mtime batch before `LIBRARY_READY` ensures Phase 1 correctly skips unchanged albums on the next run, even if the process is killed before Phase 4 completes.

---

## Phase 4 — RESOLVE (MusicBrainz)

**Goal:** enrich albums with full MB metadata. Skipped if `mb_resolve = false` or `pg_conninfo` is empty.

### Resolution strategy (per album)

```
Step 1: check_tags_for_release_id()
        → SELECT musicbrainz_release_id FROM albums WHERE id = ?
        → if set (from Phase 2 tag extraction): use directly → skip to Step 3

Step 2: find_release_by_fingerprint()   [only for untagged albums]
        → for each track (up to MB_MIN_TRACKS_FOR_MATCH):
            mb_fingerprint_generate(library_root/album_path/track_path)
            → FFmpeg decode 120 s → Chromaprint hash
            mb_acoustid_lookup(acoustid_pg_client, fingerprint)
            → AcoustID Postgres query → list of candidate release_ids
        → consensus vote (best_count / fingerprinted >= MB_MATCH_CONFIDENCE)

Step 3: mb_fetch_release(pg_client, release_id)
        → single Postgres query → mb_release_t

Step 4: match_tracks(release, local_tracks)
        → match by disc_num + track_num

Step 5: db_begin_transaction()
          db_update_album_mb()                ← album metadata
          db_update_album_artist() via db_get_or_create_artist_mb()
          for each matched track:
            db_update_track_title()
            db_set_track_artists() via db_get_or_create_artist_mb()
          db_sync_album_fts(album_id)         ← one bulk FTS sync
        db_commit()                           ← durable; visible to LibraryCache immediately
```

Each album commit is independent. A kill mid-Phase 4 leaves resolved albums in the DB and unresolved albums with their Phase 2 data intact — fully browseable. The next run's Phase 4 resumes from `mb_status IN (NOT_ATTEMPTED, HAS_RELEASE_ID)`.

### Phase 4 metadata table

| MB field | Source | Processing | SQLite column written |
|---|---|---|---|
| Release title | Postgres `release.name` | Overwrites Phase 2 folder name if present | `albums.title` |
| `musicbrainz_release_id` | Already in DB (from tags) or AcoustID consensus | — | `albums.musicbrainz_release_id` |
| `musicbrainz_release_group_id` | Postgres | — | `albums.musicbrainz_release_group_id` |
| `release_type` | Postgres | Album / Single / EP / etc. | `albums.release_type` |
| `label` | Postgres | First release label | `albums.label` |
| `catalog_number` | Postgres | — | `albums.catalog_number` |
| `barcode` | Postgres | — | `albums.barcode` |
| Release date | Postgres | Extract 4-digit year | `albums.year` (only if > 0) |
| `mb_status` | Set to `MB_STATUS_RESOLVED` | — | `albums.mb_status`, `albums.mb_resolved_at` |
| Artist name | Postgres `artist_credit.name` | `db_get_or_create_artist_mb()` | `artists.name`, `artists.musicbrainz_id`, `artists.sort_name` |
| Artist sort name | Postgres | Fill if currently NULL | `artists.sort_name` |
| Track artist credits | Postgres per recording | Replaces Phase 2 credits | `track_artists.*`, `tracks.artist_display` |
| Track title | Postgres recording title | Overwrites Phase 2 title | `tracks.title` |
| AcoustID fingerprint | Generated on-demand from audio file | 120 s Chromaprint decode | Never persisted to DB |

---

## Phase 5 — FINALIZE

```
db_checkpoint(PASSIVE)   ← final WAL flush after Phase 4 writes
→ INDEXER_COMPLETED fires
```

UI re-warms the library cache to make MB-enriched artist names, sort names, and track credits visible without a restart.

---

## Durability & Crash Recovery

| Kill point | State on restart |
|---|---|
| During Phase 2 | Partially-written album has no `last_updated_at` → Phase 1 re-queues it. No stale data visible. |
| During Phase 3 | Atlas may be incomplete but old atlas still exists → prior atlas used. Phase 2 data fully in DB. |
| After Phase 3.5 (before Phase 4) | Full library browseable. Phase 4 restarts from all unresolved albums (`mb_status != RESOLVED`). |
| Mid-Phase 4 | Resolved albums durable (each has its own commit). Unresolved albums show Phase 2 data. Next run resumes from remaining unresolved albums only. |

---

## Thread Model

```
Main thread (GTK)            Indexer worker thread         GThreadPool workers
═════════════════            ══════════════════════        ═══════════════════
indexer_scan() ──────────→  indexer_worker()
                             phase_scan()
                             phase_metadata()  ─────────→  metadata_worker() × N
                               [waits for pool]  ←────────   (per album)
                             phase_artwork()   ─────────→  artwork_worker() × N
                               [waits for pool]  ←────────
                             phase_finalize_metadata()
                             g_idle_add(LIBRARY_READY) ──→ on_indexer_event_idle()
                                                              → "library-ready" signal
                                                              → atlas reload
                                                              → cache warm
                                                              → toast
                             phase_resolve()
                             phase_finalize_resolve()
                             g_idle_add(COMPLETED) ──────→ on_indexer_event_idle()
                                                              → "completed" signal
                                                              → cache re-warm
```

- LibraryCache `db_ui` + `db_warm` are **read-only** SQLite connections. SQLite WAL allows unlimited concurrent readers alongside a single writer — Phase 4 writes never block cache reads.
- Phase 4 commits atomically per album. Each commit is immediately visible to `db_ui` / `db_warm` readers via WAL read-through.
- If the user kills the app during Phase 4, the next launch re-warms the cache from the DB; resolved albums are fully visible, unresolved show Phase 2 data.

---

## mb_status State Machine

```
NOT_ATTEMPTED (0)  ──Phase 2──→  HAS_RELEASE_ID (1)  [if MUSICBRAINZ_ALBUMID tag present]
NOT_ATTEMPTED (0)  ──Phase 4──→  RESOLVED (2) | NO_MATCH (3) | FAILED (4)
HAS_RELEASE_ID (1) ──Phase 4──→  RESOLVED (2) | FAILED (4)
```

Phase 4 queries `WHERE mb_status IN (0, 1)`. Albums with status 2/3/4 are skipped.
