# Quadrature Documentation

## Architecture

- [Overview](architecture/OVERVIEW.md) - System architecture, threading, configuration
- [Audio Engine](architecture/AUDIO_ENGINE.md) - Decoding, PipeWire integration, real-time constraints
- [Audio Cache](architecture/AUDIO_CACHE.md) - Track buffering and preloading
- [Library System](architecture/LIBRARY_SYSTEM.md) - Multi-library design, SQLite schema, portable drives
- [Library Cache](architecture/LIBRARY_CACHE.md) - In-memory cache, pointer lifetimes, UI safety
- [Indexer](architecture/INDEXER.md) - Four-phase queue-based indexer
- [Artwork Manager](architecture/ARTWORK_MANAGER.md) - Album/artist atlas system
- [Metadata](architecture/METADATA.md) - MusicBrainz enrichment pipeline
- [Axia Integration](architecture/AXIA_INTEGRATION.md) - GPIO, LWRP protocol, Livewire+

## UI Design

- [Components](ui/COMPONENTS.md) - Widget inventory
- [Conventions](ui/CONVENTIONS.md) - GTK4 patterns and coding conventions
- [Channel Strip](ui/CHANNEL_STRIP.md) - Per-channel widget layout and states
- [Library](ui/LIBRARY.md) - Library browser, filtering, detail views
- [Navigation](ui/NAVIGATION.md) - Navigation stack and routing
- [Keybinds](ui/KEYBINDS.md) - Keyboard shortcuts
