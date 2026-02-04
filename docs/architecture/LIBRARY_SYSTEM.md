# Library System

Five-phase indexer with MusicBrainz resolution. SQLite for local state, PostgreSQL for MusicBrainz + AcoustID lookups.

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
│  │  artists, fingerprints │    │  acoustid_compare2() + GIN index   │   │
│  └────────────────────────┘    └────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────┘
```

### Phase 1 — SCAN

Fast, single-threaded. Builds a work queue of changed album directories.

1. `db_get_album_mtimes_page()` → build `GHashTable` of `path → (album_id, last_updated_at)`
2. Walk watch paths recursively
3. `stat(dir)` to get current mtime
4. Lookup in hashmap: mtime matches → skip; differs or missing → queue for processing

Completes in <1 second for unchanged libraries.

### Phase 2 — METADATA

Parallel via `GThreadPool`. For each queued directory:

1. Extract metadata from audio files (FFmpeg)
2. Build folder album context (detect multi-disc, compilation, etc.)
3. Parse artist credits from tags
4. Write tracks/albums/artists to SQLite
5. Generate Chromaprint fingerprint and cache in SQLite (`tracks.chromaprint`)

### Phase 3 — ARTWORK

Parallel via `GThreadPool`. For each album in work queue:

1. Find artwork file in album directory (priority: `cover.jpg` > `folder.jpg` > `front.jpg` > `albumart.jpg`, with `.png`/`.webp` variants)
2. Fall back to embedded artwork from first audio file
3. Resize to thumbnail (48x48 default)
4. Write to atlas file (`~/.local/share/quadrature/art/`)

### Phase 4 — RESOLVE

MusicBrainz resolution via local PostgreSQL. Processes all albums with `mb_status = NOT_ATTEMPTED`.

**Two-tier resolution:**

```
Tier 1: File has MUSICBRAINZ_ALBUMID tag
        → Use that release UUID directly

Tier 2: No tags
        → Read cached fingerprints from SQLite
        → Local AcoustID PG query (acoustid_compare2 + GIN index)
        → Consensus vote across album tracks → release UUID

Then:   Fetch full release from MusicBrainz PG
        → Match tracks by position
        → Write canonical metadata to SQLite
```

No heuristic metadata matching. No HTTP. No file writes. Binary: tags exist or they don't.

**What MB resolution provides:**

- Canonical artist names and sort names (with MusicBrainz IDs)
- Canonical album title, release type, label, barcode
- Canonical track titles and recording IDs
- Proper album artist vs. track artist separation
- Compilation detection
- Release year correction

### Phase 5 — FINALIZE

Single-threaded cleanup:

- `db_set_album_mtimes_batch()` for all successfully processed albums
- Update error flags for albums with issues
- WAL checkpoint for durability

______________________________________________________________________

## Database Schema

SQLite with WAL mode. MusicBrainz columns populated by Phase 4 resolver.

```sql
CREATE TABLE artists (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    -- MB resolution (Phase 4)
    musicbrainz_id TEXT,
    sort_name TEXT
);

CREATE TABLE albums (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    artist_id INTEGER REFERENCES artists(id),
    album_artist_id INTEGER REFERENCES artists(id),
    path TEXT NOT NULL DEFAULT '',
    year INTEGER,
    last_updated_at INTEGER,          -- Delta detection (Phase 1)
    is_compilation INTEGER DEFAULT 0,
    genres TEXT,                       -- Comma-separated (set by MB resolver)
    -- MB resolution (Phase 4)
    musicbrainz_release_id TEXT,
    musicbrainz_release_group_id TEXT,
    release_type TEXT,
    label TEXT,
    barcode TEXT,
    mb_status INTEGER DEFAULT 0,      -- 0=not_attempted, 1=resolved, 2=no_match, 3=failed
    mb_resolved_at INTEGER,
    UNIQUE(title, artist_id)
);

CREATE TABLE tracks (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    album_id INTEGER REFERENCES albums(id),
    path TEXT NOT NULL UNIQUE,
    duration_ms INTEGER NOT NULL,
    track_num INTEGER,
    disc_num INTEGER NOT NULL DEFAULT 1,
    mtime INTEGER NOT NULL DEFAULT 0,
    year INTEGER DEFAULT 0,
    genre TEXT,
    metadata TEXT NOT NULL DEFAULT '{}',
    artist_display TEXT,              -- "Artist A feat. Artist B"
    -- Fingerprint cache (Phase 2)
    chromaprint TEXT,
    chromaprint_duration INTEGER DEFAULT 0,
    -- MB resolution (Phase 4)
    musicbrainz_recording_id TEXT
);

-- Multi-artist junction table
CREATE TABLE track_artists (
    track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
    artist_id INTEGER NOT NULL REFERENCES artists(id),
    role INTEGER NOT NULL DEFAULT 0,      -- 0=primary, 1=featuring
    position INTEGER NOT NULL DEFAULT 0,  -- display order
    PRIMARY KEY (track_id, artist_id)
);

-- Full-text search
CREATE VIRTUAL TABLE tracks_fts USING fts5(
    title, content='tracks', content_rowid='id'
);

-- Watch paths (user-configured scan roots)
CREATE TABLE watch_paths (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL UNIQUE,
    enabled INTEGER NOT NULL DEFAULT 1,
    last_scanned INTEGER
);

-- Indexer errors for user review
CREATE TABLE indexer_errors (
    id INTEGER PRIMARY KEY,
    path TEXT NOT NULL,
    message TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now'))
);
```

**Storage:** ~400 bytes/track with MB columns. 100k tracks ~ 40MB.

______________________________________________________________________

## Directory Format

### Single-Disc Album

```
/Artist/Album/
  cover.jpg
  01-track.flac
  02-track.flac
```

### Multi-Disc Album

```
/Artist/Album/
  cover.jpg          <- Artwork in parent folder
  CD1/
    01-track.flac
  CD2/
    01-track.flac
```

Disc folder patterns (case-insensitive): `CD1`, `CD 1`, `CD-1`, `Disc1`, `Disc 1`, `Disc-1`, `Disc One`, `D1`.

The indexer creates one album record pointing to the parent directory, extracts tracks from all disc directories, and sets `disc_num` on each track.

### Validation Rules

The indexer logs errors for these conditions:

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

______________________________________________________________________

## MusicBrainz Infrastructure

### PostgreSQL Setup

The resolver requires a self-hosted PostgreSQL instance with:

- **MusicBrainz database** — full mirror of musicbrainz.org data (artists, releases, recordings, etc.)
- **AcoustID data** — fingerprint-to-recording mappings with `acoustid_compare2()` function and GIN index

Standard MusicBrainz replication keeps the data current. The AcoustID dataset is a separate import.

### Resolution Flow

```
For each unresolved album (mb_status = 0):
    │
    ├── Read first track's tags
    │   └── Has MUSICBRAINZ_ALBUMID? ──── YES ──→ Tier 1: use release UUID
    │                                      NO
    │                                      │
    ├── Read cached fingerprints from SQLite
    │   └── For each track:
    │       └── Query local AcoustID PG (acoustid_compare2)
    │           └── Returns candidate recording_ids with scores
    │
    ├── Consensus vote: group recordings by release, pick best match
    │
    ├── Fetch full release from MusicBrainz PG
    │   └── Match tracks by position (disc_num, track_num)
    │
    └── Write to SQLite:
        ├── albums: release_id, release_group_id, release_type, label, barcode, year
        ├── tracks: recording_id, title (canonical)
        ├── artists: musicbrainz_id, sort_name (deduplicated)
        └── albums.mb_status = 1 (resolved)
```

### What Remains Without MusicBrainz

If MB resolution is disabled or no match is found, the library still works — it just uses whatever metadata was in the file tags. The indexer's tag parsing (artist splitting on `feat.`/`;`, album artist detection, compilation heuristics) provides a reasonable baseline. MB resolution upgrades that baseline to canonical data.
