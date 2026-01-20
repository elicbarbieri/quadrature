# Quadrature Development Roadmap

## Current Status

**Phase**: Early Development (Core Audio + UI Foundation)

The core audio engine is functional with 4-channel PipeWire playback, FFmpeg decoding, and real-time metering. GTK4 UI framework is in place with channel strips. Currently integrating spectrum analysis.

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
