# Quadrature Development Roadmap

## Current Status

**Phase**: Early Development (Core Audio + UI Foundation)

The core audio engine is functional with 4-channel PipeWire playback, FFmpeg decoding, and real-time metering. GTK4 UI framework is in place with channel strips. Currently integrating spectrum analysis.

---

## Library & Metadata (Active Engineering)

Design reference: `docs/architecture/METADATA.md`

### Bugs (data loss)

- [ ] **Re-index destroys MB-enriched metadata** — `upsert_track` in `src/database/db.c` unconditionally overwrites `title` and `db_set_track_artists` replaces MB credits on every re-index. Albums with `mb_status = RESOLVED` get their `artist_id`, `year`, `is_compilation` overwritten by file tags too. Resolver does not re-run (`mb_status = 1`), so data is permanently lost.
  - Fix `upsert_track`: preserve `title` / `artist_display` when `musicbrainz_recording_id IS NOT NULL`
  - Fix album upsert: preserve MB-written columns when `mb_status = RESOLVED`
  - Fix `db_set_track_artists` call in Phase 2: skip write if `position > 0` rows already exist

### Schema cleanup

- [ ] **Drop dead columns** — `albums.album_artist_id` (always equals `artist_id`), `albums.genres` (never written; genre filtering aggregates `tracks.genre` at query time), `tracks.chromaprint` + `tracks.chromaprint_duration` (see below), `tracks.metadata` (unused JSON blob)
  - Requires schema migration: `ALTER TABLE DROP COLUMN` (SQLite ≥ 3.35) or recreate-and-copy
  - Audit and remove all binding sites in `db.c`, `db_write.c`, `db_read.c`, `quadrature_database.h`

- [ ] **Drop `watch_paths` table** — vestigial in per-library design; the client holds root paths
  - Remove `db_add_watch_path`, `db_remove_watch_path`, `db_get_watch_paths`, `db_update_watch_path_scanned` and all callers

- [ ] **Move chromaprints to in-memory only** — Phase 2 generates fingerprints and passes them to Phase 4 via a `GHashTable<track_id → mb_fingerprint_t>` on the indexer context. Phase 4 re-generates from file on force-resolve retry if the entry is missing. Drop `db_get_track_fingerprint` and `db_set_track_fingerprint` entirely.

### Path storage

- [ ] **Migrate to relative paths** — `albums.path` relative to library root; `tracks.path` relative to `albums.path`. UNIQUE constraint on tracks becomes `UNIQUE(album_id, path)`.
  - Full path reconstruction: `g_canonicalize_filename(g_build_filename(track->path, NULL), g_build_filename(library_root, album->path, NULL))`
  - One-time migration query at DB open when paths are absolute (strip `library_root` prefix)
  - Update `library_cache_t` path resolution (already has `music_base`), `db_get_track_count_for_path` (currently uses `LIKE path || '/%'` with absolute paths)
  - Sibling disc tracks will have paths like `../Album Disc 2/01 Track.flac` — `g_canonicalize_filename` handles `..`

### Directory layout

- [ ] **Sibling disc detection** — albums stored as `Artist/Album Disc 1/` + `Artist/Album Disc 2/` (siblings) are currently indexed as two separate album records. Phase 1 must detect the pattern and group them into one multi-disc work item.
  - When scanning a parent directory, collect all subdirs; group those sharing a common prefix differing only by disc suffix (`Disc N`, `CD N`, `(Disc N)`)
  - Require ≥2 matching siblings to trigger grouping (avoid false positives)
  - Set synthetic `albums.path` = common prefix; track paths use `../Album Disc N/file.flac`
  - Reuse existing `work_queue_push_multi_disc` infrastructure

### New indexer modes

- [ ] **`force_resolve` flag** — re-resolves albums with `mb_status = 0` (including those manually reset from NO_MATCH/FAILED) without requiring a mtime change. Phase 4 re-fingerprints from file as needed. Expose in UI as "Retry failed lookups" that resets `mb_status` for target albums then runs indexer with this flag.

- [ ] **MB staleness sync** — separate operation (not part of normal indexing) that uses MusicBrainz PostgreSQL `last_updated` columns to find stale cached data. No file I/O.
  - Checkpoint: `MAX(albums.mb_resolved_at) WHERE mb_status = 1` — no new table needed
  - Query PG: `SELECT gid FROM release/recording/artist WHERE last_updated > to_timestamp(checkpoint)`
  - Cross-reference GIDs against `albums.musicbrainz_release_id`, `tracks.musicbrainz_recording_id`, `artists.musicbrainz_id`
  - Re-fetch + rewrite only the matching rows; release ID already known so no fingerprinting
  - Implement in `src/musicbrainz/mb_resolver.c` as a separate entry point (`mb_resolver_sync`)

### Metadata quality

- [ ] **Write MB release title to `albums.title`** — `db_update_album_mb` should accept the MB release title and write it to `albums.title`. Folder name preserved in `albums.path`. Guarded by `mb_status = RESOLVED` check (so re-index doesn't restore folder name). See `src/musicbrainz/mb_resolver.c` and `src/database/db_write.c`.

- [ ] **Multi-artist album credits (`album_artists` table)** — `mb_resolver.c` currently uses only `release.artists[0]`. Add `album_artists` junction table mirroring `track_artists` (`album_id, artist_id, position, join_phrase, credited_name`). Update resolver to write all release artist credits. Update library cache warming to load `album_artists`.

---

## In Progress

- [ ] Spectrum analyzer integration (cavacore FFT, ring buffer IPC)
- [ ] Level meter widget refinement (peak/RMS/hold display)
- [ ] Spectrum display widget (24-bar frequency visualization)
- [ ] Channel strip polish (integrate all metering widgets)

---

## Phase 1: Core Playback Polish

### Audio Engine
- [ ] Crossfade between channels
- [ ] Gapless playback / auto-next on EOF
- [ ] Cue/headphone monitoring output
- [ ] Master output bus with volume control
- [ ] Sample rate conversion verification

### UI Completion
- [ ] Waveform display widget
- [ ] Position timeline with click-to-seek
- [ ] Keyboard shortcuts (space=play/pause, etc.)
- [ ] Drag-and-drop files to channels
- [ ] Error dialogs for load failures

### Library Integration
- [ ] Library browser panel (search, browse by artist/album)
- [ ] Directory scanning and indexing
- [ ] Metadata extraction from audio files
- [ ] Drag tracks from library to channels

---

## Phase 2: Broadcast Features

### Timing & Display
- [ ] Countdown timers per channel
- [ ] Wall-clock display with NTP sync
- [ ] Cue points and markers
- [ ] Loop regions
- [ ] Remaining time display

### Control Interfaces
- [ ] GPIO support (hardware buttons, tally lights)
- [ ] Network control protocol (TCP/UDP commands)
- [ ] Serial/RS-232 for broadcast consoles

### Logging & Compliance
- [ ] Broadcast log (timestamped play history)
- [ ] Export logs for compliance/billing
- [ ] Now-playing metadata export

---

## Phase 3: Professional Features

### Audio Processing
- [ ] Per-channel EQ (at minimum HPF/LPF)
- [ ] Compressor/limiter
- [ ] Normalization/auto-gain
- [ ] Ducking for voiceovers

### Advanced Playback
- [ ] Playlist management per channel
- [ ] Voice tracking support
- [ ] Automation system hooks
- [ ] BWF (Broadcast Wave Format) with timecode

### UI Enhancements
- [ ] Settings/preferences dialog
- [ ] Theme/skin support
- [ ] Multi-monitor/detachable layouts
- [ ] Accessibility improvements

---

## Phase 4: Reliability & Polish

### Testing
- [ ] Integration test suite (full stack)
- [ ] Audio pipeline stress tests
- [ ] UI automation tests
- [ ] Cross-platform build verification

### Documentation
- [ ] API documentation (Doxygen)
- [ ] User manual
- [ ] Deployment guide
- [ ] Architecture overview

### Stability
- [ ] Crash recovery / session restore
- [ ] Graceful degradation on audio errors
- [ ] Performance profiling and optimization
- [ ] Memory leak analysis

---

## Future Ideas

- [ ] Mobile remote control app
- [ ] Cloud configuration sync
- [ ] Plugin system for custom decoders/effects
- [ ] Multi-user/networked operation
- [ ] Advanced automation integration

---

## Completed

### Infrastructure
- [x] Project structure and build system (CMake + Nix flake)
- [x] Core engine lifecycle (create/start/stop/destroy)
- [x] Configuration management (INI-style key-value)
- [x] Logging system (ERROR/WARN/INFO/DEBUG levels)
- [x] Unit test framework (Criterion)

### Audio Engine
- [x] 4-channel audio pipeline with PipeWire
- [x] FFmpeg decoder integration (MP3, FLAC, OGG, WAV, AAC, M4A)
- [x] Real-time peak/RMS metering with atomic updates
- [x] Lock-free ring buffer for spectrum IPC
- [x] Per-channel volume and pan controls
- [x] Mute per channel

### User Interface
- [x] GTK4 application framework
- [x] Main window with paned layout
- [x] Channel strip widget with transport controls
- [x] File load dialog with format filtering
- [x] Position/duration display

### Music Library
- [x] SQLite database schema (artists, albums, tracks)
- [x] FTS5 full-text search capability
- [x] WAL mode for concurrent access

---

## Architecture Notes

### Real-Time Safety
All audio callback code is lock-free. Use atomics for UI-to-audio thread communication. Never allocate memory or take locks in the audio path.

### Key Source Files
| File | Purpose |
|------|---------|
| `src/audio/audio_pipeline.c` | Core playback engine, metering |
| `src/audio/audio_ringbuf.c` | Lock-free SPSC ring buffer |
| `src/audio/audio_spectrum.c` | FFT spectrum analysis thread |
| `src/ui/channel_strip.c` | Per-channel UI widget |
| `src/ui/level_meter.c` | Peak/RMS meter widget |
| `src/ui/spectrum_display.c` | Frequency bar display |
| `src/library/library_db.c` | SQLite music database |

### Dependencies
| Library | Purpose |
|---------|---------|
| PipeWire | Audio I/O and routing |
| FFmpeg | Format decoding |
| GTK4 | User interface |
| SQLite3 | Music library |
| cavacore | Spectrum analysis (FFT) |
| Criterion | Unit testing |
