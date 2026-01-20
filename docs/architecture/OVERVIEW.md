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

## Design Principles

- **No volume control** - Console controls all levels
- **No mixing** - Each channel outputs independently
- **No effects** - Processing on console or outboard
- **Read-only metering** - Display only, doesn't modify audio

## Threading

| Thread       | Purpose                                | Priority  |
| ------------ | -------------------------------------- | --------- |
| Main         | GTK4 UI, user input                    | Normal    |
| Decode Pool  | FFmpeg decode, resample (Audio Cache)  | Normal    |
| Audio        | PipeWire callbacks, sample output      | Real-time |
| Spectrum     | FFT analysis (reads from ring buffer)  | Normal    |

Communication via lock-free ring buffers and atomics. Audio thread reads pre-decoded buffers from cache—no decoding on real-time thread. See [Audio Cache](AUDIO_CACHE.md) and [Audio Engine](AUDIO_ENGINE.md) for details.

## Library Sources

| Source          | Indexed By                  | Database Location                  |
| --------------- | --------------------------- | ---------------------------------- |
| Primary NAS     | `quadrature-indexer` daemon | NFS share                          |
| Portable drives | Client UI (in-process)      | On drive: `.quadrature/library.db` |

See [Library System](LIBRARY_SYSTEM.md) for details.

## Configuration

```ini
[audio]
sample_rate = 48000
buffer_size = 1024

[channel.1]
sink = console_input_1

[channel.2]
sink = console_input_2

[channel.3]
sink = console_input_3

[channel.4]
sink = console_input_4

[library]
db_path = /mnt/nas/music/library.db
music_base = /mnt/nas/music
```

## Platform Requirements

- Linux with PipeWire
- GTK4, FFmpeg, SQLite3, FFTW3
- Filesystems: ext4, XFS, Btrfs, or ZFS (fanotify support required)
