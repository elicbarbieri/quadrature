# Metadata Architecture

Which source wins for each database field: file tags (Phase 2) vs. MusicBrainz (Phase 4).
For the indexer pipeline, schema, and path layout see LIBRARY_SYSTEM.md.

---

## ⚠ Minimal Storage Principle — CRITICAL

**Store the minimum data needed. Derive everything else on demand.**

- Do NOT add columns "just in case" or for convenience caching.
- Every new column must justify its existence: what query requires it that cannot be answered from existing data?
- MusicBrainz data: if it can be derived from `albums.musicbrainz_release_id` + `(disc_num, track_num)`, **do not store it**.
  - Per-track `musicbrainz_recording_id` was deliberately removed. Tracks are addressed by position within the release.
- If you feel the urge to cache a derived value in the DB, stop and ask: is this a hot path? Can I compute it at query time instead?

---

## Album Resolution States

`albums.mb_status`:

| Value | Constant      | Meaning                                         |
| ----- | ------------- | ----------------------------------------------- |
| `0`   | NOT_ATTEMPTED | File tags only. Resolution not yet run.         |
| `1`   | RESOLVED      | Full MusicBrainz release applied.               |
| `2`   | NO_MATCH      | Attempted; no confident match found.            |
| `3`   | FAILED        | Error during resolution attempt.                |

Resolution runs once per album. To retry NO_MATCH/FAILED albums: reset `mb_status = 0`
(via UI or manual SQL), then run the indexer with `force_resolve = true`.

---

## Field Provenance

### `albums`

| Column                         | Phase 2 (file tags)               | Phase 4 (MusicBrainz)                                                                    |
| ------------------------------ | --------------------------------- | ---------------------------------------------------------------------------------------- |
| `title`                        | Folder name                       | **Overwritten** with MB release title. Preserved on re-index if `mb_status = RESOLVED`. |
| `artist_id`                    | ALBUMARTIST tag → ARTIST fallback | **Overwritten** (MB release artist, deduped by MBID). Preserved on re-index if `mb_status = RESOLVED`. |
| `is_compilation`               | `0`                               | **Overwritten**: `1` if Various Artists (`89ad4ac3-…`). Preserved on re-index.          |
| `year`                         | First track DATE tag              | **Overwritten** if MB year > 0. Preserved on re-index.                                  |
| `musicbrainz_release_id`       | —                                 | **Written**                                                                              |
| `musicbrainz_release_group_id` | —                                 | **Written**                                                                              |
| `release_type`                 | —                                 | **Written** (Album, EP, Single, Broadcast, …)                                            |
| `label`                        | —                                 | **Written** (first `release_label` entry)                                                |
| `catalog_number`               | —                                 | **Written** (first non-null `release_label.catalog_number`)                              |
| `barcode`                      | —                                 | **Written** (`release.barcode`)                                                          |
| `mb_status`                    | `0`                               | **Written**: 1 / 2 / 3                                                                   |
| `mb_resolved_at`               | —                                 | **Written** (Unix timestamp)                                                             |
| `path`                         | Relative to library root          | Never changed                                                                            |
| `last_updated_at`              | Written in Phase 5                | Never changed                                                                            |

### `tracks`

| Column                     | Phase 2 (file tags)                | Phase 4 (MusicBrainz)                                                                                |
| -------------------------- | ---------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `title`                    | TITLE tag (filename fallback)      | **Overwritten** with MB recording title. Preserved on re-index if `albums.mb_status = RESOLVED`.    |
| `artist_display`           | Raw ARTIST tag                     | **Overwritten** by `db_set_track_artists`. Preserved on re-index if album MB-resolved.              |
| `genre`                    | GENRE tag (NULL if absent)         | **Not overwritten**. Always refreshed from file on re-index.                                         |
| `track_num`                | TRACKNUMBER tag                    | **Not overwritten**. Always refreshed.                                                               |
| `disc_num`                 | DISCNUMBER tag or disc folder name | **Not overwritten**. Always refreshed.                                                               |
| `year`                     | DATE tag                           | **Not overwritten**. Always refreshed (album year corrected by MB instead).                          |
| `duration_ms`              | Audio stream (FFmpeg)              | **Not overwritten**. Always refreshed.                                                               |
| `path`                     | Relative to `albums.path`          | Never changed                                                                                        |
| `mtime`                    | `stat()` mtime                     | Never changed                                                                                        |

### `track_artists`

| Phase   | What is written                                                                                                           |
| ------- | ------------------------------------------------------------------------------------------------------------------------- |
| Phase 2 | Single entry: `position=0, join_phrase="", credited_name=NULL` — raw ARTIST tag. Skipped if MB credits already present. |
| Phase 4 | **Fully replaced** with MB recording artist credits: one row per credit with `position`, `join_phrase`, and `credited_name` when alias differs from canonical name. |

`tracks.artist_display` is rebuilt atomically inside `db_set_track_artists`.

### `artists`

| Phase   | Behaviour                                                                                                             |
| ------- | --------------------------------------------------------------------------------------------------------------------- |
| Phase 2 | `INSERT OR IGNORE` by name (case-insensitive). No MBID, no `sort_name`.                                              |
| Phase 4 | Dedup by `musicbrainz_id` first, then by name. Writes `sort_name` and `musicbrainz_id`. Enriches in-place — no duplicates. |

---

## Artist Credit Display

`tracks.artist_display` is the canonical human-readable credit for the UI,
built by concatenating `track_artists` in position order:

```
display = ""
for each row in track_artists ORDER BY position:
    display += (credited_name ?? artists.name) + join_phrase
```

- Solo: `"Daft Punk"` (`join_phrase = ""`)
- Featured: `"Daft Punk feat. Pharrell Williams & Nile Rodgers"`

`credited_name` is non-NULL only when the MB credit alias differs from `artists.name`
(e.g. `"Weeknd"` credited vs canonical `"The Weeknd"`).

For unresolved tracks, `artist_display` is the raw ARTIST file tag and `track_artists`
has a single entry with `join_phrase = ""`.

---

## Metadata DB (`quadrature-metadata.sqlite`)

A separate SQLite database per library root that stores MusicBrainz recording relations
(producers, remixers, vocalists, engineers, etc.). Created only after Phase 4 runs. If
absent, the UI shows no relation data — no crash.

**Why separate?** `quadrature.sqlite` runs a 64MB page cache + 256MB mmap for playback and
browsing. Relation data is queried at most on detail-view open, so it gets a 128MB cache +
128MB mmap only while a query is in flight. Between queries the metadata DB is closed
entirely — zero persistent RAM.

### Schema

Mirrors the MusicBrainz relational model for SQLite:

```sql
-- Bridge: (release_mbid, disc_num, track_num) → recording_mbid
CREATE TABLE recordings (
    recording_mbid  TEXT PRIMARY KEY,
    release_mbid    TEXT NOT NULL,  -- albums.musicbrainz_release_id
    disc_num        INTEGER NOT NULL,
    track_num       INTEGER NOT NULL
);

-- Mirrors link_type (gid, name, description)
CREATE TABLE link_types (
    link_type_gid  TEXT PRIMARY KEY,
    name           TEXT NOT NULL,   -- e.g. "producer", "remixer", "vocal"
    description    TEXT
);

-- Mirrors artist (gid, name, sort_name) + artist_type.name
CREATE TABLE artists (
    artist_mbid  TEXT PRIMARY KEY,
    name         TEXT NOT NULL,
    sort_name    TEXT,
    artist_type  TEXT              -- "Person", "Group", "Orchestra", etc.
);

-- Mirrors l_artist_recording + link, one row per relationship
-- attributes: comma-separated link_attribute_type.name values
-- entity0_credit: credited name override for this specific link
CREATE TABLE recording_links (
    id              INTEGER PRIMARY KEY,
    recording_mbid  TEXT NOT NULL REFERENCES recordings(recording_mbid),
    artist_mbid     TEXT NOT NULL REFERENCES artists(artist_mbid),
    link_type_gid   TEXT NOT NULL REFERENCES link_types(link_type_gid),
    entity0_credit  TEXT,  -- NULL = use artist.name
    attributes      TEXT   -- comma-separated; NULL if none
);

CREATE INDEX idx_rl_recording   ON recording_links(recording_mbid);
CREATE INDEX idx_rl_artist      ON recording_links(artist_mbid, link_type_gid);
CREATE INDEX idx_recordings_position ON recordings(release_mbid, disc_num, track_num);
```

`recording_mbid` lives ONLY in this DB — not in `quadrature.sqlite`. The UI bridges
via `(release_mbid, disc_num, track_num)`, the same natural key used throughout.

### Public API

See `include/quadrature/quadrature_metadata.h`.

Key patterns:
- **Write path (Phase 4):** `db_meta_open()` → `db_meta_begin()` → upsert rows →
  `db_meta_commit()` → `db_meta_checkpoint()` → `db_meta_close()`
- **Read path (UI):** `db_meta_open_readonly()` → `db_meta_get_recording_mbid()` →
  `db_meta_get_links()` → `db_meta_links_free()` → `db_meta_close()`
- `db_meta_open_readonly()` returns `QUADRATURE_ERROR_FILE_NOT_FOUND` if Phase 4 never ran.
