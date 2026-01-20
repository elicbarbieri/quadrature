# Quadrature Library Format

This document describes the expected directory structures for music libraries and the validation rules enforced by the indexer.

## Supported Directory Structures

### Single-Disc Album

The most common format - a folder containing audio files and optional artwork:

```
/Artist/Album/
  cover.jpg          <- Artwork (optional)
  01-track.flac
  02-track.flac
  ...
```

Artwork filenames searched (in priority order):
1. `cover.jpg`, `cover.png`, `cover.webp`
2. `folder.jpg`, `folder.png`, `folder.webp`
3. `front.jpg`, `front.png`, `front.webp`
4. `albumart.jpg`, `albumart.png`, `albumart.webp`

If no external artwork is found, embedded artwork from the first audio file is extracted.

### Multi-Disc Album

For albums spanning multiple discs, use a parent folder containing disc subdirectories:

```
/Artist/Album/
  cover.jpg          <- Artwork MUST be in parent folder
  CD1/               <- Disc subdirectory
    01-track.flac
    02-track.flac
  CD2/               <- Disc subdirectory
    01-track.flac
    02-track.flac
```

**Supported disc folder naming patterns** (case-insensitive):
- `CD1`, `CD 1`, `CD-1`
- `Disc1`, `Disc 1`, `Disc-1`
- `Disc One`, `Disc Two`, etc. (1-10)
- `D1`, `d1`

The indexer will:
1. Detect disc subdirectories by naming pattern
2. Create **one album record** pointing to the parent directory
3. Extract tracks from all disc directories
4. Set `disc_num` on each track based on the folder name
5. Look for artwork in the **parent directory only**

## Validation Rules

The indexer logs errors for the following conditions to help users fix their library structure.

### Mixed Content

**Error**: `Album has both tracks and disc folders - move tracks into disc folders`

The parent directory contains both audio files AND disc subdirectories. This is ambiguous - are the loose tracks part of disc 1, a bonus disc, or misplaced?

**Fix**: Move all audio files into appropriate disc folders.

```
BAD:
/Artist/Album/
  bonus.flac         <- Loose track
  CD1/
    01-track.flac
  CD2/
    01-track.flac

GOOD:
/Artist/Album/
  CD1/
    01-track.flac
  CD2/
    01-track.flac
  CD3/               <- Or bonus disc
    bonus.flac
```

### Orphan Disc Folder

**Error**: `Single disc folder 'CD1' found - remove disc folder or add more discs`

A directory contains only one disc subdirectory (e.g., just `CD1`). This likely indicates either a misnamed folder or incomplete organization.

**Fix**: Either remove the unnecessary disc folder level, or add the missing disc folders.

```
BAD:
/Artist/Album/
  CD1/
    01-track.flac

GOOD (single disc):
/Artist/Album/
  01-track.flac

GOOD (multi-disc):
/Artist/Album/
  CD1/
    01-track.flac
  CD2/
    01-track.flac
```

### Non-Sequential Discs

**Error**: `Non-sequential disc folders (missing Disc N)`

The disc numbers skip a value (e.g., CD1 and CD3 exist, but CD2 is missing).

**Fix**: Verify all disc folders are present or rename them to be sequential.

### Artwork in Disc Folder

**Error**: `Artwork found in disc folder - move to album root`

Album artwork was found inside a disc subdirectory instead of the parent album folder. For multi-disc albums, artwork should be at the album level.

**Fix**: Move the artwork file to the parent album folder.

```
BAD:
/Artist/Album/
  CD1/
    cover.jpg        <- Wrong location
    01-track.flac
  CD2/
    01-track.flac

GOOD:
/Artist/Album/
  cover.jpg          <- Correct location
  CD1/
    01-track.flac
  CD2/
    01-track.flac
```

### Too Deep Nesting

**Error**: `Tracks found more than 1 level deep - flatten structure`

Audio files are nested more than one level below the album directory. Quadrature expects tracks directly in the album folder or one level deep in disc folders.

**Fix**: Flatten the directory structure.

```
BAD:
/Artist/Album/
  CD1/
    Side A/          <- Too deep
      01-track.flac

GOOD:
/Artist/Album/
  CD1/
    01-track.flac
```

### Empty Disc Folder

**Error**: `Empty disc folder 'CD1' - add tracks or remove folder`

A disc subdirectory exists but contains no audio files.

**Fix**: Add the missing audio files or remove the empty folder.

## Best Practices

1. **Consistent naming**: Use the same disc folder naming pattern throughout your library (e.g., always `CD1`, `CD2` or always `Disc 1`, `Disc 2`)

2. **Album artwork**: Place artwork in the album root folder with a standard name (`cover.jpg` is most widely supported)

3. **Track numbering**: Use consistent track number tags within each disc - the indexer will sort by `disc_num` then `track_num`

4. **Compilation albums**: The indexer detects compilations automatically when tracks have multiple different artists but share an album name
