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

Toplevel views use the view name only. Detail-to-detail navigation includes the entity name so the user knows which page they will return to.

## Navigation Stack

The detail view maintains an internal nav stack that tracks the chain of views the user navigated through. Each entry stores:

- **type**: toplevel view, artist detail, or album detail
- **id**: entity ID (0 for toplevel views)
- **view_name**: display label for the back button (e.g., "Search", "Artist Detail - Daft Punk")

### Stack operations by navigation source

**From toplevel view (Search/Artists/Albums):**
- Stack is cleared (any stale history is discarded)
- One toplevel entry is pushed with the source view name
- Label: "Back to Search" / "Back to Artists" / "Back to Albums"

**From another detail page (artist link, album card, appears-on row):**
- Current detail state is pushed onto existing stack
- Label: "Back to Artist Detail - {name}" or "Back to Album Detail - {title}"

**From channel strip (clicking artist/album in the player):**
- If not already in detail view: current toplevel view is pushed, preserving existing stack
- If already in detail view: current detail state is pushed onto existing stack
- This preserves the user's browsing context so they can back out to wherever they were

### Stack clearing

Clicking any toplevel nav bar button (Search, Artists, Albums) clears the detail nav stack. This prevents stale back-navigation chains from accumulating when the user abandons a detail browsing session by clicking a nav bar item.

## Back Button Activation

The back button activates on single left-click.

Escape also triggers back navigation when in the detail view. Escape priority:

1. Close errors overlay (if visible)
2. Navigate back in detail view (if in detail view)
3. Return focus to search entry (if in search view)
