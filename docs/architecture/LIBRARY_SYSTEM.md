# Library System

Five-phase indexer with MusicBrainz resolution. Each library root gets its own `quadrature.sqlite` and `artwork/` directory. PostgreSQL is used for MusicBrainz + AcoustID lookups only — all results are written to the local SQLite. Audio files are never modified.

---

## ⚠ Minimal Storage Principle — CRITICAL

**The SQLite schema must stay as lean as possible. Every column must earn its place.**

Rules:
1. **No redundant identifiers.** If a piece of data is derivable from `(album_id + disc_num + track_num)` or from the MB release already stored on the album, do NOT add a column for it. This is why `tracks` has no `musicbrainz_recording_id` — it is always addressable via `albums.musicbrainz_release_id` + position.
2. **No convenience caches.** If you can compute a value at query time in < 1ms, don't persist it.
3. **Before adding any new column**, ask: *what breaks if this column does not exist?* If the answer is "nothing, it's just faster/easier", reject it.
4. **Phase-level data flows one way.** File tags → Phase 2. MusicBrainz → Phase 4. Phase 4 data is cached in the album row, not duplicated into track rows.

Violating this principle creates schema debt that compounds on every re-index and every migration.

---

## Read-Only Library

**Quadrature never modifies files in the library directory.**

All enrichment is stored exclusively in locations at the library root:

```
{library_root}/
  quadrature.sqlite           ← all track/album/artist metadata (RAM-heavy: 64MB cache, 256MB mmap)
  quadrature-metadata.sqlite  ← MusicBrainz recording relations (created only after Phase 4 runs)
  artwork/                    ← thumbnail atlas files (96px-{unix_time}.atlas)
```

The library's audio files and folder structure are a read-only source of truth.
Quadrature will never rename a folder, rewrite a tag, or touch any file it did
not create.

`quadrature-metadata.sqlite` is optional — if it doesn't exist, the UI simply shows
no relation data. It is created and written by Phase 4 after a successful MB resolution.
See `quadrature_metadata.h` for the public API and `METADATA.md` → "Metadata DB" for the schema.

---

## Multi-Library Architecture

Each library root is an independent unit with its own `quadrature.sqlite` and
`artwork/` directory. No shared state between libraries. A library is fully
portable: move the directory tree (including `quadrature.sqlite`) and all
metadata moves with it.

The client holds the list of registered library root paths. The database does
not store its own absolute path.

---

## Indexer Architecture

```
┌──────────────────────────────────────────────────────────────────────────┐
│                              INDEXER                                      │
│                                                                          │
│  Phase 1 ─ SCAN          stat() + hashmap → work queue of changed dirs  │
│  Phase 2 ─ METADATA      GThreadPool: FFmpeg extract + fingerprint      │
│  Phase 3 ─ ARTWORK       GThreadPool: image resize → atlas              │
│  Phase 4 ─ RESOLVE       MusicBrainz: tags or fingerprint → PG lookup  │
│  Phase 5 ─ FINALIZE      Batch mtime update, error flags, checkpoint    │
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

Fast, single-threaded. Builds a work queue of changed album directories.

1. `db_get_album_mtimes_page()` → build `GHashTable` of `path → (album_id, last_updated_at)`
2. Walk library root recursively
3. `stat(dir)` to get current mtime
4. Lookup in hashmap: mtime matches → skip; differs or missing → queue for processing
5. Sibling disc detection: subdirectories sharing a common prefix differing only by a disc
   suffix (`Disc N`, `CD N`, `(Disc N)`) are grouped into one multi-disc work item with a
   synthetic canonical path equal to the common prefix. Requires ≥2 matching siblings.

Completes in <1 second for unchanged libraries.

If `force_resolve = true`: also queue albums where `mb_status = 0` for Phase 4
re-resolution. These skip Phase 2 metadata re-extraction if mtime is unchanged —
fingerprints are re-generated from file in Phase 4 as needed.

### Phase 2 — METADATA

Parallel via `GThreadPool`. For each queued directory:

1. Open each audio file via FFmpeg; extract tags + stream duration
2. Tags read: `title`, `artist`, `album_artist`, `album`, `track`, `disc`, `date/year`, `genre`
3. Title fallback: filename (minus extension) if TITLE tag absent
4. Write `artists`, `albums`, `tracks`, `track_artists` to SQLite

For existing tracks (path already in DB):
- ALWAYS update: `duration_ms`, `mtime`, `track_num`, `disc_num`, `year`, `genre`, `path`
- PRESERVE if `albums.mb_status = RESOLVED`: `tracks.title`, `tracks.artist_display`, `track_artists` rows
- PRESERVE if `albums.mb_status = RESOLVED`: `albums.artist_id`, `albums.is_compilation`, `albums.year`, and all `albums.musicbrainz_*` columns

If fingerprinting enabled (`options.fingerprint_tracks = true`):
- Generate chromaprint: decode first 120s → mono 11025Hz via libchromaprint
- Hold **in memory** on the indexer context; passed to Phase 4 within the same run
- NOT stored in the database

**Pre-tagged files:** Files carrying `MUSICBRAINZ_ALBUMID` / `MUSICBRAINZ_TRACKID` tags
(e.g. from MusicBrainz Picard) are indexed normally in Phase 2. The MB IDs are read
directly from the file in Phase 4 Tier 1, which skips fingerprinting. These albums
resolve instantly with zero chromaprint cost.

### Phase 3 — ARTWORK

Parallel via `GThreadPool`. For each album in work queue:

1. Discover artwork: `cover.*` → `folder.*` → `front.*` → `albumart.*` → embedded
2. Resize to 96px thumbnail via libvips
3. Write PNG entry to `{library_root}/artwork/96px-{unix_time}.atlas`

### Phase 4 — RESOLVE

Optional. Runs when `mb_resolve = true` and PostgreSQL is configured.
Processes albums where `mb_status = 0`.

**Tier 1 — File tag fast-path:**
- Read first track's file for `MUSICBRAINZ_ALBUMID`
  (tries: `MUSICBRAINZ_ALBUMID`, `"MusicBrainz Album Id"`, space variants)
- If found → use release UUID directly; skip fingerprinting entirely

**Tier 2 — Fingerprint consensus** (only if Tier 1 found nothing):
- Read in-memory chromaprints from Phase 2 of this run
  (or re-generate from file if this is a `force_resolve` retry run)
- Query local AcoustID PostgreSQL via `acoustid_compare2()` + GIN index
- Consensus vote across up to 3 tracks per album
- ≥80% must agree on same release; below threshold → `mb_status = NO_MATCH`

**On match (either tier):**
- Fetch full release from local MusicBrainz PostgreSQL (no HTTP)
- Two-pass track matching:
    Pass 1: exact `(disc_num, position)`
    Pass 2: score = `duration_sim×0.6 + title_jaccard×0.4`; threshold 0.5
- Write enriched metadata to SQLite (see METADATA.md — Field Provenance)
- `mb_status = RESOLVED`, `mb_resolved_at = now()`
- Fetch all artist–recording links for this release from PG (`l_artist_recording`)
- Write links to `quadrature-metadata.sqlite` (producers, remixers, vocalists, engineers, etc.)
  See METADATA.md → "Metadata DB" for schema details.

**On failure:** `mb_status = NO_MATCH` or `FAILED`

### MB Staleness Sync

Separate operation (not part of normal indexing). Uses MusicBrainz PostgreSQL
`last_updated` columns to detect stale cached data. No file I/O, no fingerprinting.

Checkpoint derived from existing data — no new table needed:

```sql
SELECT MAX(mb_resolved_at) FROM albums WHERE mb_status = 1
```

Sync flow:

```
checkpoint = MAX(albums.mb_resolved_at) WHERE mb_status = 1

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

For each stale album:   re-run Phase 4 fetch + write (release ID already known, no fingerprinting)
For each stale track:   re-fetch recording + artist credits; update tracks/track_artists/artist_display
For each stale artist:  update artists.name, artists.sort_name in-place
```

Cost is proportional to the number of MB edits since the last sync — typically a small
fraction of total albums for a stable library.

### Phase 5 — FINALIZE

Single-threaded cleanup:

- `db_set_album_mtimes_batch()` for all successfully processed albums
- Clear `indexer_errors` for successfully rescanned directories
- WAL checkpoint for durability

---

## Database Schema

SQLite with WAL mode. MusicBrainz columns populated by Phase 4 resolver.

```sql
CREATE TABLE artists (
    id             INTEGER PRIMARY KEY,
    name           TEXT NOT NULL UNIQUE COLLATE NOCASE,
    sort_name      TEXT,               -- NULL until Phase 4 (MB resolution)
    musicbrainz_id TEXT
);

CREATE TABLE albums (
    id              INTEGER PRIMARY KEY,
    title           TEXT NOT NULL,     -- folder name until Phase 4 writes MB title
    artist_id       INTEGER REFERENCES artists(id),
    path            TEXT NOT NULL UNIQUE,  -- relative to library root; delta detection key
    year            INTEGER,
    is_compilation  INTEGER DEFAULT 0,
    last_updated_at INTEGER,           -- directory mtime written in Phase 5; Phase 1 delta key
    -- MB fields (all NULL until Phase 4)
    musicbrainz_release_id       TEXT,
    musicbrainz_release_group_id TEXT,
    release_type    TEXT,              -- Album, EP, Single, Broadcast, ...
    label           TEXT,
    catalog_number  TEXT,
    barcode         TEXT,
    mb_status       INTEGER DEFAULT 0, -- 0=not_attempted 1=resolved 2=no_match 3=failed
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
    -- within albums.musicbrainz_release_id. See "Minimal Storage Principle" below.
    UNIQUE(album_id, path)
);

CREATE TABLE track_artists (
    track_id      INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    artist_id     INTEGER NOT NULL REFERENCES artists(id),
    position      INTEGER NOT NULL DEFAULT 0,
    join_phrase   TEXT NOT NULL DEFAULT '',  -- " feat. ", " & ", "" for last artist
    credited_name TEXT,                      -- non-NULL only when alias ≠ artists.name
    PRIMARY KEY (track_id, artist_id)
);

CREATE VIRTUAL TABLE tracks_fts USING fts5(
    title, content='tracks', content_rowid='id'
);

CREATE TABLE indexer_errors (
    id         INTEGER PRIMARY KEY,
    path       TEXT NOT NULL,
    message    TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);
```

**Storage:** ~400 bytes/track with MB columns. 100k tracks ~ 40MB.

---

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

| Error | Condition | Fix |
|-------|-----------|-----|
| Mixed content | Album has both loose tracks and disc folders | Move tracks into disc folders |
| Orphan disc folder | Only one disc subdirectory exists | Remove folder level or add more discs |
| Non-sequential discs | Disc numbers skip a value | Rename to be sequential |
| Artwork in disc folder | Artwork found inside disc subdir | Move to album root |
| Too deep nesting | Tracks >1 level below album dir | Flatten structure |
| Empty disc folder | Disc subdirectory has no audio files | Add tracks or remove folder |

### Artwork Discovery

Priority order per album directory:

1. `cover.{jpg,png,webp}`
2. `folder.{jpg,png,webp}`
3. `front.{jpg,png,webp}`
4. `albumart.{jpg,png,webp}`
5. Embedded artwork from first audio file (FFmpeg extraction)

---

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

---

## MusicBrainz Infrastructure

### PostgreSQL Setup

The resolver requires a self-hosted PostgreSQL instance with:

- **MusicBrainz database** — full mirror of musicbrainz.org data (artists, releases, recordings, etc.)
- **AcoustID data** — fingerprint-to-recording mappings with `acoustid_compare2()` function and GIN index

Standard MusicBrainz replication keeps the data current. The AcoustID dataset is a separate import.

### Data Sources

All MB data comes via libpq — no HTTP calls during resolution.

| Data                                  | PostgreSQL tables                                                               |
| ------------------------------------- | ------------------------------------------------------------------------------- |
| Release metadata (title, date, type)  | `release` + `release_group` + `release_status` + `release_country`             |
| Label + catalog number                | `release_label` + `label`                                                       |
| Album artist credits                  | `release.artist_credit` → `artist_credit_name` → `artist`                      |
| Track list (position, disc, duration) | `medium` → `track` → `recording`                                                |
| Recording artist credits              | Batch join: `medium` → `track` → `recording` → `artist_credit_name` → `artist` |

AcoustID fingerprint lookup uses the same PostgreSQL instance (`acoustid` schema alongside `musicbrainz`).

### What Remains Without MusicBrainz

If MB resolution is disabled or no match is found, the library still works — it just uses
whatever metadata was in the file tags. The indexer's tag parsing (artist splitting on
`feat.`/`;`, album artist detection, compilation heuristics) provides a reasonable baseline.
MB resolution upgrades that baseline to canonical data.
