# Architecture Overview

4-channel audio player for broadcast. Outputs to PipeWire sinks feeding a broadcast console. Console handles mixing, levels, routing.

## System Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                         GTK4 UI                                 │
│  ┌───────────────────────────┬─────────────────────────────────┐│
│  │  Library Panel            │    Channel Strips (1-4)         ││
│  │  - Search (all sources)   │    - Transport, meters, spectrum││
│  │  - Browse artists/albums  │                                 ││
│  │  - Libraries tab          │                                 ││
│  └───────────────────────────┴─────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────┘
         │                                       │
         │ audio_cache_load(track_id)            │ audio_pipeline_set_player_track()
         v                                       v
┌─────────────────────────┐             ┌─────────────────────────────────────┐
│      AUDIO CACHE        │             │            AUDIO ENGINE             │
│                         │             │                                     │
│  FFmpeg decode ──────>  │             │  Player 1   Player 2   Player 3   Player 4
│  Resample to 48kHz      │ ──────────> │     │          │          │          │
│  LRU cache (512MB)      │ try_acquire │  Scrubber  Scrubber  Scrubber  Scrubber
│                         │             │  Meter     Meter     Meter     Meter
└─────────────────────────┘             └─────┼──────────┼──────────┼──────────┼─┘
                                              v          v          v          v
                                         PipeWire   PipeWire   PipeWire   PipeWire
                                              └──────────┴──────────┴──────────┘
                                                              │
                                                    Broadcast Console
```

## Indexer Pipeline

Five-phase pipeline handles library scanning, metadata extraction, and MusicBrainz resolution:

```
Phase 1 — SCAN       Fast directory walk, stat() + hashmap delta detection
Phase 2 — METADATA   Parallel FFmpeg metadata extraction + Chromaprint fingerprinting
Phase 3 — ARTWORK    Parallel image processing → thumbnail atlas
Phase 4 — RESOLVE    MusicBrainz resolution via local PostgreSQL (two-tier)
Phase 5 — FINALIZE   Batch DB updates (mtimes, error flags, WAL checkpoint)
```

Phase 4 uses a self-hosted MusicBrainz + AcoustID PostgreSQL database. Two-tier resolution: if the file has MB tags (e.g. from Picard), use them directly; otherwise, match cached fingerprints against local AcoustID via `acoustid_compare2()` + consensus voting. All resolved metadata written to SQLite — never modifies library files.

See [Library System](LIBRARY_SYSTEM.md) for details.

## Design Principles

- **No volume control** - Console controls all levels
- **No mixing** - Each channel outputs independently
- **No effects** - Processing on console or outboard
- **Read-only metering** - Display only, doesn't modify audio
- **Read-only library** - Never modifies files in scanned paths

## Threading

| Thread       | Purpose                                    | Priority  |
| ------------ | ------------------------------------------ | --------- |
| Main         | GTK4 UI, user input                        | Normal    |
| Decode Pool  | FFmpeg decode, resample (Audio Cache)      | Normal    |
| Audio        | PipeWire callbacks, sample output          | Real-time |
| Spectrum     | FFT analysis (reads from ring buffer)      | Normal    |
| Indexer Pool | Metadata extraction, fingerprinting        | Normal    |
| MB Resolver  | PostgreSQL queries for MusicBrainz data    | Normal    |

Communication via lock-free ring buffers and atomics. Audio thread reads pre-decoded buffers from cache — no decoding on real-time thread. See [Audio Cache](AUDIO_CACHE.md) and [Audio Engine](AUDIO_ENGINE.md) for details.

## Configuration

```ini
# ~/.config/quadrature/settings.ini

[Channel1]
device = alsa_output.pci-0000_00_1f.3.analog-stereo
enabled = true
output_format = 1

[Display]
show_spectrum = true
time_warning_threshold = 30000

[Library]
auto_scan_on_startup = true
process_artwork = true
indexer_threads = 0
art_thumb_size = 48

[MusicBrainz]
pg_conninfo = host=localhost dbname=musicbrainz_db user=musicbrainz
auto_resolve = true
```

## Library Storage

Each library root is self-contained:

```
{library_root}/
  quadrature.sqlite           ← all metadata for tracks under this root
  quadrature-metadata.sqlite  ← MusicBrainz recording relations (after Phase 4 runs)
  artwork/                    ← thumbnail atlas files (96px-{unix_time}.atlas)
  Artist/Album/               ← audio files (never modified by quadrature)
```

Multiple libraries can be registered. Each has its own SQLite database and artwork directory — no shared state between libraries.

`quadrature-metadata.sqlite` is written by Phase 4 on successful MB resolution and read on-demand by the UI. If absent, the UI shows no relation data — no crash. See [Metadata Architecture](METADATA.md) for schema details.

## Platform Requirements

- Linux with PipeWire
- GTK4, GLib, FFmpeg, SQLite3, FFTW3, libvips, Rubberband
- Chromaprint (audio fingerprinting)
- libpq (PostgreSQL client for MusicBrainz + AcoustID)
- Self-hosted PostgreSQL with MusicBrainz + AcoustID data
