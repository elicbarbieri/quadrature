# Keyboard Shortcuts

Comprehensive keyboard shortcut reference for the Quadrature UI.

## Global Shortcuts

These work everywhere in the application.

| Key      | Action                                    | Context |
| -------- | ----------------------------------------- | ------- |
| `Ctrl+F` | Focus search view (All filter)            | Global  |
| `Escape` | Clear/cancel current operation or go back | Global  |

## Channel Control

Control channel strips from anywhere in the UI.

| Key        | Action                                        |
| ---------- | --------------------------------------------- |
| `Ctrl+1-4` | Toggle focus on channel 1-4                   |
| `1-4`      | Load selected/hovered track to channel N      |
| `Enter`    | Load selected track to next available channel |
| `Space`    | Play/Pause focused channel                    |

**Note:** Number key shortcuts (1-4, Enter) only work when a track is selected or hovered in library views.

## Library Navigation

Navigate between main library views.

| Key       | Action                              |
| --------- | ----------------------------------- |
| `Ctrl+F`  | Go to Search view                   |
| `Ctrl+A`  | Go to Artists view                  |
| `Ctrl+B`  | Go to Albums view                   |
| `Up/Down` | Navigate list items                 |
| `Left`    | Go back / collapse detail view      |
| `Right`   | Open detail / expand item           |
| `Enter`   | Open selected item (detail or load) |
| `Escape`  | Go back to previous view            |

## Search View

Available when in the Search view.

| Key       | Action                                   |
| --------- | ---------------------------------------- |
| `Ctrl+F`  | Focus search input, set filter to All    |
| `Ctrl+A`  | Set search filter to Artists only        |
| `Ctrl+B`  | Set search filter to Albums only         |
| `Ctrl+S`  | Set search filter to Songs only          |
| `Escape`  | Clear search query / unfocus search      |
| `Up/Down` | Navigate search results                  |
| `Enter`   | Open selected item (artist/album detail) |
| `1-4`     | Load selected song to channel 1-4        |

## Artists View

Available when browsing the Artists list or Artist detail view.

| Key           | Action                            |
| ------------- | --------------------------------- |
| `Up/Down`     | Navigate artist list              |
| `Enter`       | Open artist detail view           |
| `Right`       | Open artist detail view           |
| `Left`        | Back to artist list (from detail) |
| `Escape`      | Back to artist list (from detail) |
| `Shift+Enter` | Shuffle all artist tracks         |
| `/`           | Focus filter/search box           |

**In Artist Detail View:**

| Key       | Action                             |
| --------- | ---------------------------------- |
| `Up/Down` | Navigate album list within artist  |
| `Enter`   | Open selected album detail         |
| `1-4`     | Load selected track to channel 1-4 |

## Albums View

Available when browsing the Albums list or Album detail view.

| Key           | Action                           |
| ------------- | -------------------------------- |
| `Up/Down`     | Navigate album list              |
| `Enter`       | Open album detail view           |
| `Right`       | Open album detail view           |
| `Left`        | Back to album list (from detail) |
| `Escape`      | Back to album list (from detail) |
| `Shift+Enter` | Shuffle album tracks             |
| `/`           | Focus filter/search box          |

**In Album Detail View:**

| Key       | Action                                 |
| --------- | -------------------------------------- |
| `Up/Down` | Navigate track list                    |
| `Enter`   | Load selected track with album context |
| `1-4`     | Load selected track to channel 1-4     |

## Details View

The Details view is context-aware and shows either Album or Artist detail.

| Key           | Action                             |
| ------------- | ---------------------------------- |
| `Up/Down`     | Navigate tracks/albums in detail   |
| `Enter`       | Load track or open album           |
| `Left`        | Back to previous view              |
| `Escape`      | Back to previous view              |
| `1-4`         | Load selected track to channel 1-4 |
| `Shift+Enter` | Shuffle current album/artist       |

## Channel Strip View

When focus is on a channel strip.

| Key          | Action                                       |
| ------------ | -------------------------------------------- |
| `Space`      | Play/Pause current track                     |
| `S`          | Stop playback                                |
| `Left/Right` | Seek backward/forward 5 seconds              |
| `[/]`        | Previous/Next track in album                 |
| `R`          | Toggle Repeat                                |
| `A`          | Toggle Autoplay                              |
| `P`          | Toggle Preview (PFL)                         |
| `Q`          | Queue channel (single press)                 |
| `Q` (double) | Exit Queue/On-Air mode                       |
| `-/+`        | Decrease/Increase shuttle speed              |
| `0`          | Reset shuttle to 1.0x                        |
| `K`          | Cycle shuttle mode (OFF → KEYLOCK → PITCHED) |

## Quick Reference

### Most Common Actions

| Task                        | Shortcut               |
| --------------------------- | ---------------------- |
| Search for music            | `Ctrl+F`, type query   |
| Browse artists              | `Ctrl+A`               |
| Browse albums               | `Ctrl+B`               |
| Load track to channel 1     | Hover track, press `1` |
| Focus channel 2 for loading | `Ctrl+2`               |
| Play/pause focused channel  | `Space`                |
| Go back                     | `Escape` or `Left`     |
| Navigate lists              | `Up/Down`              |
| Open detail view            | `Enter` or `Right`     |

### Loading Tracks

Tracks always load with **full album context**, enabling auto-advance and previous/next navigation.

| Method                 | Result                                               |
| ---------------------- | ---------------------------------------------------- |
| Press `1-4`            | Load to specific channel (if track selected/hovered) |
| Press `Enter`          | Load to next available channel                       |
| Right-click track      | Load to focused/preview channel                      |
| Drag & drop to channel | Load to target channel                               |

**Focused Channel:** Set with `Ctrl+1-4`. Right-click and Enter use focused channel when available.

## Conventions

- **Modifier keys:** `Ctrl`, `Shift`, `Alt`
- **Arrow keys:** `Up`, `Down`, `Left`, `Right`
- **Special keys:** `Enter`, `Space`, `Escape`, `Tab`
- **Range notation:** `1-4` means keys 1, 2, 3, or 4
- **Double press:** `Q` (double) means press Q twice quickly

## Context-Sensitive Behavior

Many shortcuts behave differently based on context:

- **Number keys (1-4)**: Load tracks only when a track is selected/hovered in a library view
- **Enter**: Opens detail view when on list items, loads tracks when on tracks
- **Escape**: Clears search in Search view, goes back in detail views, clears focus elsewhere
- **Space**: Play/pause only works when channel strip has focus

When in doubt, `Escape` returns to a neutral state or previous view.
