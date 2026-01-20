# Quadrature

A professional 4-channel audio player for broadcast studios, written in C with GTK4.

## Features

- **4-Channel Playback**: Independent control of four audio channels
- **Broadcast Console Integration**: Start/stop control via GPIO, serial, and network
- **Preview/PFL**: Monitor audio before going live
- **Professional Formats**: FFmpeg-based decoding (MP3, FLAC, WAV, OGG, AAC, BWF)
- **Music Library**: SQLite-based with full-text search
- **Spectrum Analyzer**: Real-time FFT visualization

## Quick Start

```bash
nix develop        # Enter dev environment
make run           # Build and run with UI
make test          # Run tests
make clean         # Clean build
```

## Dependencies

- FFmpeg (libavformat, libavcodec, libavutil, libswresample)
- PipeWire
- GTK4 (for UI)
- SQLite3
- Criterion (for tests)
- FFTW3

## Version Control

This project uses [Jujutsu (jj)](https://github.com/martinvonz/jj) for version control.

## Documentation

See [docs/](docs/) for architecture and design documentation.

## Status

Early development. See [TODO.md](TODO.md) for the roadmap.

## License

[License to be determined]
