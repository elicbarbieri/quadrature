# Detail View Navigation

Back navigation for the unified detail view (album/artist detail pages).

## Back Button Label Format

Labels are context-aware, showing where the user will return to:

| Navigation context | Back label |
| --- | --- |
| Came from Search | `Back to Search` |
| Came from Artists view | `Back to Artists` |
| Came from Albums view | `Back to Albums` |
| Came from artist detail | `Back to Artist Detail - Daft Punk` |
| Came from album detail | `Back to Album Detail - Random Access Memories` |

Toplevel views use the view name only. Detail-to-detail navigation includes the entity name so the user knows which page they'll return to.

## Navigation Stack

The detail view maintains an internal nav stack (`NavEntry[]`) that tracks the chain of views the user navigated through. Each entry stores:

- **type**: `NAV_ENTRY_VIEW` (toplevel), `NAV_ENTRY_ARTIST`, or `NAV_ENTRY_ALBUM`
- **id**: Entity ID (0 for toplevel views)
- **view_name**: Display label for the back button (e.g., `"Search"`, `"Artist Detail - Daft Punk"`)

### Stack operations by navigation source

**From toplevel view (Search/Artists/Albums):**
- Stack is **cleared** (any stale history is discarded)
- One `NAV_ENTRY_VIEW` is pushed with the source view name
- Label: `"Back to Search"` / `"Back to Artists"` / `"Back to Albums"`

**From another detail page (artist link, album card, appears-on row):**
- Current detail state is **pushed** onto existing stack
- Label: `"Back to Artist Detail - {name}"` or `"Back to Album Detail - {title}"`

**From channel strip (clicking artist/album in the player):**
- If not already in detail view: current toplevel view is **pushed** as `NAV_ENTRY_VIEW`, preserving existing stack
- If already in detail view: current detail state is **pushed** onto existing stack
- This preserves the user's browsing context so they can back out to wherever they were

### Stack clearing

Clicking any **toplevel nav bar button** (Search, Artists, Albums) clears the detail nav stack. This prevents stale back-navigation chains from accumulating when the user abandons a detail browsing session by clicking a nav bar item.

## Back Button Activation

The back button is a `GtkButton` connected to the `"clicked"` signal. It activates on **single left-click**.

**Escape key** also triggers back navigation when in the detail view:

1. Close errors overlay (if visible)
2. **Navigate back in detail view** (if in detail view)
3. Return focus to search entry (if in search view)

## Implementation

### Key files

- `src/ui/library/detail_view.c` - Nav stack, navigate/go_back functions
- `src/ui/window.c` - Window-level navigation handlers, Escape action
- `src/ui/library/internal.h` - `NavEntry` type, detail view public API

### Back navigation flow

```
Back button click (or Escape)
  │
  ├─ library_unified_detail_go_back()
  │   ├─ Stack empty → return FALSE
  │   ├─ NAV_ENTRY_VIEW → pop, return FALSE (exit detail)
  │   ├─ NAV_ENTRY_ARTIST → load artist, update label, return TRUE
  │   └─ NAV_ENTRY_ALBUM → load album, update label, return TRUE
  │
  └─ If returned FALSE → on_back callback
      └─ ui_window_navigate_to(previous_view)
```

The `on_back` callback is only invoked when internal navigation is exhausted. It must NOT call `library_unified_detail_go_back()` again (the button handler already consumed the stack entry).
