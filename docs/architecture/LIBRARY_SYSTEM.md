# Library System

Eight-phase indexer with MusicBrainz resolution and artist enrichment. Each library gets its own `quadrature.sqlite`, `quadrature-metadata.sqlite`, `quadrature-bios.sqlite`, and `artwork/` directory. PostgreSQL is used for MusicBrainz + AcoustID lookups only — all results are written to local SQLite. Audio files are never modified.

______________________________________________________________________

## ⚠ Minimal Storage Principle — CRITICAL

**The SQLite schema must stay as lean as possible. Every column must earn its place.**

Rules:

1. **No redundant identifiers.** If a piece of data is derivable from `(album_id + disc_num + track_num)` or from the MB release already stored on the album, do NOT add a column for it. This is why `tracks` has no `musicbrainz_recording_id` — it is always addressable via `albums.musicbrainz_release_id` + position.
1. **No convenience caches.** If you can compute a value at query time in < 1ms, don't persist it.
1. **Before adding any new column**, ask: *what breaks if this column does not exist?* If the answer is "nothing, it's just faster/easier", reject it.
1. **Phase-level data flows one way.** File tags → Phase 2. MusicBrainz → Phase 4. Phase 4 data is cached in the album row, not duplicated into track rows.

Violating this principle creates schema debt that compounds on every re-index and every migration.

______________________________________________________________________

## Read-Only Library

**Quadrature never modifies files in the library directory.**

Each library has a `library_root` (music files, always read-only) and a `data_root`
(databases + artwork). `data_root` defaults to `library_root` but can be overridden
in settings (e.g. for read-only network drives).

All enrichment is stored at the data root:

```
{data_root}/
  quadrature.sqlite           ← all track/album/artist metadata (RAM-heavy: 64MB cache, 256MB mmap)
  quadrature-metadata.sqlite  ← MusicBrainz recording relations + release info (created after Phase 6)
  quadrature-bios.sqlite      ← Artist biographies from Wikipedia (created after Phase 8)
  artwork/                    ← thumbnail atlas files (48px-artwork-{unix_time}.atlas)

~/.local/share/quadrature/atlas/
  artists.atlas               ← global UUID-keyed artist thumbnail atlas (shared across all libraries)
  artists.atlas.lock          ← flock() write serialization
```

The library's audio files and folder structure are a read-only source of truth.
Quadrature will never rename a folder, rewrite a tag, or touch any file it did
not create.

`quadrature-metadata.sqlite` is optional — if it doesn't exist, the UI simply shows
no relation data. Created and written by Phase 6 after a successful MB resolution.
`quadrature-bios.sqlite` is optional — created by Phase 8 after Wikipedia lookups.
See `quadrature/metadata.h` for the public API and `METADATA.md` for the schemas.

______________________________________________________________________

## Multi-Library Architecture

Each library is an independent unit with its own `quadrature.sqlite`, metadata DBs,
and `artwork/` directory. The only shared state is the global artist atlas.

**Content vs metadata boundary:** Library bitmask filtering applies to content queries
(which albums, tracks, and artist listings to display). Metadata resolution — MBID
lookups, artist artwork, bios — always uses `LIBRARY_MASK_ALL`. The MBID is a
universal key: if an artist is known in any library, its art and bio are available
everywhere. Library filtering only controls whether the artist *appears* in the UI;
once shown, all enrichment data is library-agnostic.

Libraries are configured in `settings.ini` as `library_config_t` entries with:

- `path` — music folder root (required)
- `data_path` — database/artwork location (optional, defaults to `path`)
- `name` — display name (optional, defaults to basename)
- `library_index` — stable slot ID persisted across sessions (used in global entity IDs)
- Per-library toggles: `mb_resolve`, `acoustid`, `fanart`, `wikipedia` (each -1/0/1: inherit/off/on)

A library is fully portable: move the data directory (including `quadrature.sqlite`)
and all metadata moves with it. The database does not store its own absolute path.

The library cache uses **global IDs** encoded as `LIBRARY_MAKE_GLOBAL_ID(bitmap_index, local_id)`
to address entities across libraries (upper 16 bits = library index, lower 48 bits = local SQLite ID).
See [Library Cache](LIBRARY_CACHE.md) and [Deduplication](DEDUPLICATION.md).

______________________________________________________________________

## Indexer Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                              INDEXER                                      │
│                                                                          │
│  Phase 1 ─ SCAN          stat() + hashmap → work queue of changed dirs  │
│  Phase 2 ─ METADATA      GThreadPool: FFmpeg tag extract (no decode)    │
│  Phase 3 ─ FINALIZE      Batch mtime flush + WAL checkpoint             │
│         ── INDEXER_LIBRARY_UPDATED ── library-cache reload → views refresh│
│  Phase 4 ─ ARTWORK       GThreadPool: image resize → atlas              │
│         ── INDEXER_ARTWORK_UPDATED ── atlas reload → views refresh        │
│  Phase 5 ─ FINGERPRINT   Chromaprint fingerprinting (on-demand)         │
│  Phase 6 ─ RESOLVE       MusicBrainz PG lookup + metadata write         │
│         ── INDEXER_LIBRARY_UPDATED ── library-cache reload → views refresh│
│  Phase 7 ─ ARTIST_ART    Fetch artist images from fanart.tv → atlas     │
│         ── INDEXER_ARTWORK_UPDATED ── artist atlas reload → views refresh │
│  Phase 8 ─ ARTIST_BIO    Fetch artist bios from Wikipedia via Wikidata  │
│         ── INDEXER_COMPLETED ──                                           │
│                                                                          │
│              │                              │                            │
│              v                              v                            │
│  ┌────────────────────────┐    ┌────────────────────────────────────┐   │
│  │  SQLite (WAL mode)     │    │  PostgreSQL (self-hosted)          │   │
│  │  tracks, albums,       │    │  MusicBrainz + AcoustID data       │   │
│  │  artists               │    │  acoustid_compare2() + GIN index   │   │
│  └────────────────────────┘    └────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────┘
```

### Phase 1 — SCAN

Fast, single-threaded. Builds a work queue of changed album directories and detects orphans.

1. `db_get_album_mtimes_page()` → build `GHashTable` of `path → (album_id, last_updated_at, last_updated_size)`
1. Walk library root with `nftw(FTW_PHYS)` (symlinks not followed — see INDEXER.md)
1. `stat(dir)` → current mtime + file count + total size
1. Lookup in hashmap: mtime AND size match → skip; either differs or missing → queue
1. Remove matched entry from hashmap (mark as "seen")
1. Sibling disc detection: subdirectories sharing a common prefix differing only by a disc
   suffix (`Disc N`, `CD N`, `(Disc N)`) are grouped into one multi-disc work item with a
   synthetic canonical path equal to the common prefix. Requires ≥2 matching siblings.
1. **Orphan pruning:** after walk, remaining hashmap entries = albums deleted from disk.
   `db_prune_orphan_albums()` deletes these albums, their tracks (→ track_artists CASCADE),
   and cleans FTS tables. Subsequent `db_prune_orphan_artists()` in Phase 3 removes any
   artists left without track or album references.

Completes in \<1 second for unchanged libraries.

If `force_resolve = true`: also queue albums where `mb_status = 0` for Phase 4
re-resolution. These skip Phase 2 metadata re-extraction if mtime is unchanged —
fingerprints are re-generated from file in Phase 4 as needed.

### Phase 2 — METADATA

Parallel via `GThreadPool`. For each queued directory:

1. Open each audio file via FFmpeg; extract tags + stream duration
1. Tags read: `title`, `artist`, `album_artist`, `album`, `track`, `disc`, `date/year`, `genre`
1. Title fallback: filename (minus extension) if TITLE tag absent
1. Write `artists`, `albums`, `tracks`, `track_artists` to SQLite

For existing tracks (path already in DB):

- ALWAYS update: `duration_ms`, `mtime`, `track_num`, `disc_num`, `year`, `genre`, `path`
- PRESERVE if `albums.mb_status = RESOLVED`: `tracks.title`, `tracks.artist_display`, `track_artists` rows
- PRESERVE if `albums.mb_status = RESOLVED`: `albums.artist_id`, `albums.is_compilation`, `albums.year`, and all `albums.musicbrainz_*` columns

**No fingerprinting in Phase 2.** Chromaprint fingerprinting is done on-demand in
Phase 5/6 — only for albums that need it and only if MB resolution is enabled.

**Pre-tagged files:** Files carrying `MUSICBRAINZ_ALBUMID` tags (e.g. from MusicBrainz
Picard) have the release ID stored directly in Phase 2 (`mb_status = HAS_RELEASE_ID`).
Phase 6 uses this directly — no fingerprinting needed. These albums resolve instantly.

### Phase 3 — FINALIZE (Metadata)

Single-threaded. Runs immediately after Phase 2, before `INDEXER_LIBRARY_UPDATED`:

- `db_set_album_mtimes_batch()` for all successfully processed albums
- Prune orphan errors for directories that no longer exist
- WAL checkpoint for durability

Running the mtime batch before the library-updated signal ensures Phase 1 correctly
skips unchanged albums on the next run, even if the process is killed before Phase 6.

### Phase 4 — ARTWORK

Parallel via `GThreadPool`. For each album in work queue:

1. Discover artwork: `cover.*` → `folder.*` → `front.*` → `albumart.*` → embedded
1. Resize to thumbnail via libvips (default 48px)
1. Write entry to `{data_root}/artwork/48px-artwork-{unix_time}.atlas`

Pure image processing — no DB metadata writes.

### Phase 5 — FINGERPRINT

Parallel Chromaprint generation for albums needing acoustic identification.
Only runs when `mb_resolve = true` and PostgreSQL is configured.

- Decode first 30 seconds of up to 4 tracks per album → mono 11025Hz via libchromaprint
- Fingerprints held in memory, passed to Phase 6
- NOT stored in the database

### Phase 6 — RESOLVE

MusicBrainz resolution. Processes albums where `mb_status IN (NOT_ATTEMPTED, HAS_RELEASE_ID)`.

**Resolution strategy (per album):**

1. Check `albums.musicbrainz_release_id` — if set (from Phase 2 tag extraction or prior run),
   use directly → skip to step 3

1. `find_release_by_fingerprint()` — fallback chain for untagged albums:

   - **ISRC lookup:** batch query ISRCs from file tags against MusicBrainz PG
   - **Solr text search:** album title + artist name query with duration validation
   - **AcoustID fingerprint:** query local AcoustID PG via `acoustid_compare2()` + GIN index,
     consensus vote across up to 4 tracks per album (≥80% must agree on same release)

1. `mb_fetch_all_batch()` — single PG call fetches release metadata, artist credits,
   recordings, and artist–recording links for up to 50 albums per round-trip

1. Match MB tracks to local tracks:

   - Pass 1: exact `(disc_num, position)`
   - Pass 2: score = `duration_sim×0.6 + title_jaccard×0.4`; threshold 0.5

1. Write enriched metadata to SQLite (see METADATA.md — Field Provenance)

   - `mb_status = RESOLVED`, `mb_resolved_at = now()`
   - Write recording relations and release info to `quadrature-metadata.sqlite`
     (producers, remixers, vocalists, engineers, release type, label, catalog number, etc.)

**On failure:** `mb_status = NO_MATCH` or `FAILED`

After all albums are processed, orphan artists (from Phase 2 that were replaced by
corrected MusicBrainz entries) are pruned.

### Phase 7 — ARTIST_ART

Fetches artist images from fanart.tv for all artists with MusicBrainz IDs.

- Downloads artist thumbnails via HTTP (rate-limited, 500ms between requests)
- Builds/updates the global artist atlas at `~/.local/share/quadrature/atlas/artists.atlas`
- Uses `flock()` on `artists.atlas.lock` for write serialization across concurrent indexer runs
- Atlas is UUID-keyed (binary MusicBrainz UUIDs) — shared across all libraries
- Each library run deduplicates: copies art from other libraries before fetching, writes merged atlas

### Phase 8 — ARTIST_BIO

Fetches artist biographies from Wikipedia via Wikidata for all artists with MusicBrainz IDs.

- Looks up Wikidata entity via MBID → fetches Wikipedia summary
- Writes to `{data_root}/quadrature-bios.sqlite` (separate DB so deleting metadata DB
  for re-resolve doesn't destroy expensive-to-refetch bios)
- Rate-limited (250ms between requests)

Both Phase 7 and 8 outputs are queried by MBID, not by library — art and bios are
available regardless of the active library filter (see Multi-Library Architecture above).

### MB Staleness Sync

Separate operation (not part of normal indexing). Uses MusicBrainz PostgreSQL
`last_updated` columns to detect stale cached data. No file I/O, no fingerprinting.

Checkpoint derived from existing data — no new table needed:

```sql
SELECT MAX(mb_resolved_at) FROM albums WHERE mb_status = 2
```

Sync flow:

```
checkpoint = MAX(albums.mb_resolved_at) WHERE mb_status = 2

-- Find stale releases/groups in PG
SELECT gid::text FROM release       WHERE last_updated > to_timestamp(checkpoint)
SELECT gid::text FROM release_group WHERE last_updated > to_timestamp(checkpoint)

-- Cross-reference against albums.musicbrainz_release_id in SQLite
-- Find stale recordings in PG
SELECT gid::text FROM recording WHERE last_updated > to_timestamp(checkpoint)

-- Cross-reference via (disc_num, track_num) against albums.musicbrainz_release_id — no per-track MBID stored
-- Find stale artists in PG
SELECT gid::text FROM artist WHERE last_updated > to_timestamp(checkpoint)

-- Cross-reference against artists.musicbrainz_id in SQLite

For each stale album:   re-run Phase 6 fetch + write (release ID already known, no fingerprinting)
For each stale track:   re-fetch recording + artist credits; update tracks/track_artists/artist_display
For each stale artist:  update artists.name, artists.sort_name in-place
```

Cost is proportional to the number of MB edits since the last sync — typically a small
fraction of total albums for a stable library.

______________________________________________________________________

## Database Schema

SQLite with WAL mode. MusicBrainz columns populated by Phase 6 resolver.

Note: `release_type`, `label`, `catalog_number`, `barcode` are stored in the
metadata DB (`quadrature-metadata.sqlite` → `releases` table), NOT in the main DB.
See [Metadata Architecture](METADATA.md) for the metadata DB schema.

```sql
CREATE TABLE artists (
    id             INTEGER PRIMARY KEY,
    name           TEXT NOT NULL UNIQUE COLLATE NOCASE,
    musicbrainz_id TEXT,
    sort_name      TEXT               -- NULL until Phase 6 (MB resolution)
);

CREATE TABLE albums (
    id              INTEGER PRIMARY KEY,
    title           TEXT NOT NULL,     -- folder name until Phase 6 writes MB title
    artist_id       INTEGER REFERENCES artists(id),
    path            TEXT NOT NULL DEFAULT '',  -- relative to library root; delta detection key
    year            INTEGER,
    is_compilation  INTEGER DEFAULT 0,
    last_updated_at INTEGER,           -- directory mtime written in Phase 3; Phase 1 delta key
    last_updated_size INTEGER,        -- file count + total bytes; Phase 1 secondary delta signal
    -- MB fields (all NULL until Phase 6)
    musicbrainz_release_id       TEXT,
    musicbrainz_release_group_id TEXT,
    mb_status       INTEGER DEFAULT 0, -- see mb_status values below
    mb_resolved_at  INTEGER
);

CREATE TABLE tracks (
    id           INTEGER PRIMARY KEY,
    title        TEXT NOT NULL,
    album_id     INTEGER REFERENCES albums(id),
    path         TEXT NOT NULL,        -- relative to albums.path; playback key
    duration_ms  INTEGER NOT NULL,
    track_num    INTEGER,
    disc_num     INTEGER NOT NULL DEFAULT 1,
    mtime        INTEGER NOT NULL DEFAULT 0,
    year         INTEGER DEFAULT 0,
    genre        TEXT,
    artist_display TEXT,               -- denormalized "Artist A feat. B"; rebuilt atomically
    -- NOTE: no per-track MBID stored. Tracks are addressed via (disc_num, track_num)
    -- within albums.musicbrainz_release_id. See "Minimal Storage Principle" above.
    UNIQUE(album_id, path)
);

-- Multi-artist junction table (WITHOUT ROWID: composite PK, small rows)
CREATE TABLE track_artists (
    track_id      INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    artist_id     INTEGER NOT NULL REFERENCES artists(id),
    position      INTEGER NOT NULL DEFAULT 0,
    join_phrase   TEXT NOT NULL DEFAULT '',  -- " feat. ", " & ", "" for last artist
    PRIMARY KEY (track_id, artist_id)
) WITHOUT ROWID;

-- Full-text search (standalone FTS5 — manually synced via db_sync_album_fts)
-- content-sync is not viable: tracks_fts columns (title, artist, album) are derived
-- from JOINs (artist_display, album title), not direct column mappings.
CREATE VIRTUAL TABLE tracks_fts USING fts5(title, artist, album);
CREATE VIRTUAL TABLE artists_fts USING fts5(name);
CREATE VIRTUAL TABLE albums_fts USING fts5(title, artist);

CREATE TABLE indexer_errors (
    id              INTEGER PRIMARY KEY,
    path            TEXT NOT NULL,
    error_code      INTEGER NOT NULL DEFAULT 0,  -- structured enum (see below)
    phase           INTEGER NOT NULL DEFAULT 0,  -- which indexer phase produced this error
    severity        INTEGER NOT NULL DEFAULT 2,  -- 1=warn, 2=error, 3=fatal
    message         TEXT NOT NULL,
    created_at      INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),
    scan_generation INTEGER NOT NULL DEFAULT 0
);
-- Error codes: 100=file_unreadable, 101=permission_denied, 200=ffmpeg_decode,
-- 201=ffmpeg_no_streams, 300=artwork_missing, 301=artwork_corrupt,
-- 400=mb_no_match, 401=mb_pg_error, 500=network_error
```

### mb_status Values

| Value | Constant         | Meaning                                                      |
| ----- | ---------------- | ------------------------------------------------------------ |
| `0`   | `NOT_ATTEMPTED`  | No MB work done; no release ID in DB                         |
| `1`   | `HAS_RELEASE_ID` | Release UUID found in Picard tags (Phase 2); not yet fetched |
| `2`   | `RESOLVED`       | Fully resolved: MB PG data fetched and written to SQLite     |
| `3`   | `NO_MATCH`       | Resolution attempted but no confident match found            |
| `4`   | `FAILED`         | Resolution attempted but errored                             |

Phase 6 queries `WHERE mb_status IN (0, 1)`. Albums with status 2/3/4 are skipped.

**Storage:** ~300 bytes/track. 100k tracks ~ 30MB.

______________________________________________________________________

## Directory Format

### Single-Disc Album

```
Artist/Album/
  cover.jpg
  01-track.flac
  02-track.flac
```

### Multi-Disc Album (Nested)

```
Artist/Album/
  cover.jpg        ← artwork in parent folder
  CD1/
    01-track.flac
  CD2/
    01-track.flac
```

Disc folder patterns (case-insensitive): `CD1`, `CD 1`, `CD-1`, `Disc1`, `Disc 1`, `Disc-1`, `Disc One`, `D1`.

The indexer creates one album record pointing to the parent directory, extracts tracks from
all disc directories, and sets `disc_num` on each track.

### Multi-Disc Album (Sibling)

```
Artist/
  Album Disc 1/01-track.flac
  Album Disc 2/01-track.flac
```

Phase 1 detects subdirectories sharing a common prefix differing only by a disc suffix and
groups them into a single album work item. A synthetic canonical path equal to the common
prefix is used:

```
albums.path = "Artist/Album"                    ← synthetic; no actual directory at this path
tracks.path = "../Album Disc 1/01-track.flac"
              "../Album Disc 2/01-track.flac"
```

`g_canonicalize_filename` resolves the `..` when building the playback path. The
`albums.path` UNIQUE constraint still holds — the synthetic prefix is unique within the
library.

### Validation Rules

| Error                  | Condition                                    | Fix                                   |
| ---------------------- | -------------------------------------------- | ------------------------------------- |
| Mixed content          | Album has both loose tracks and disc folders | Move tracks into disc folders         |
| Orphan disc folder     | Only one disc subdirectory exists            | Remove folder level or add more discs |
| Non-sequential discs   | Disc numbers skip a value                    | Rename to be sequential               |
| Artwork in disc folder | Artwork found inside disc subdir             | Move to album root                    |
| Too deep nesting       | Tracks >1 level below album dir              | Flatten structure                     |
| Empty disc folder      | Disc subdirectory has no audio files         | Add tracks or remove folder           |

### Artwork Discovery

Priority order per album directory:

1. `cover.{jpg,png,webp}`
1. `folder.{jpg,png,webp}`
1. `front.{jpg,png,webp}`
1. `albumart.{jpg,png,webp}`
1. Embedded artwork from first audio file (FFmpeg extraction)

______________________________________________________________________

## Path Layout

Paths in the database are **relative** to avoid coupling to the library's absolute location.
Absolute paths silently break whenever a library is moved; relative paths travel with it.

```
albums.path  — relative to library root
               e.g.  "Jazz/Miles Davis/Kind of Blue"

tracks.path  — relative to albums.path
               simple case:  "01 So What.flac"
               nested disc:  "CD1/01 So What.flac"
               sibling disc: "../Kind of Blue (Disc 2)/01 Flamenco Sketches.flac"
```

To reconstruct the full path for playback or `stat()`:

```c
char* full = g_canonicalize_filename(
    g_build_filename(track->path, NULL),
    g_build_filename(library_root, album->path, NULL)
);
// g_canonicalize_filename resolves any "../" components cleanly
```

______________________________________________________________________

## MusicBrainz Infrastructure

### PostgreSQL Setup

The resolver requires a self-hosted PostgreSQL instance with:

- **MusicBrainz database** — full mirror of musicbrainz.org data (artists, releases, recordings, etc.)
- **AcoustID data** — fingerprint-to-recording mappings with `acoustid_compare2()` function and GIN index

Standard MusicBrainz replication keeps the data current. The AcoustID dataset is a separate import.

### Data Sources

All MB data comes via libpq — no HTTP calls during resolution.

| Data                                  | PostgreSQL tables                                                              |
| ------------------------------------- | ------------------------------------------------------------------------------ |
| Release metadata (title, date, type)  | `release` + `release_group` + `release_status` + `release_country`             |
| Label + catalog number                | `release_label` + `label`                                                      |
| Album artist credits                  | `release.artist_credit` → `artist_credit_name` → `artist`                      |
| Track list (position, disc, duration) | `medium` → `track` → `recording`                                               |
| Recording artist credits              | Batch join: `medium` → `track` → `recording` → `artist_credit_name` → `artist` |

AcoustID fingerprint lookup uses the same PostgreSQL instance (`acoustid` schema alongside `musicbrainz`).

### What Remains Without MusicBrainz

If MB resolution is disabled or no match is found, the library still works — it just uses
whatever metadata was in the file tags. The indexer's tag parsing (artist splitting on
`feat.`/`;`, album artist detection, compilation heuristics) provides a reasonable baseline.
MB resolution upgrades that baseline to canonical data.

Phases 7 and 8 (artist art + bios) also depend on MusicBrainz IDs — artists without
MBIDs will not have thumbnails or biographies.
