# Library System

Dual-mode architecture: NAS daemon + client-side portable drive indexing.

## Architecture

```
┌─────────────────────────────────┬───────────────────────────────────────┐
│   PRIMARY NAS LIBRARY           │   PORTABLE DRIVE LIBRARIES            │
│   (Server-side indexing)        │   (Client-side indexing)              │
│                                 │                                       │
│  quadrature-indexer daemon      │  Client UI (GTK4)                     │
│  • fanotify watching            │  • GVolumeMonitor detection           │
│  • Delta scanning               │  • Background thread indexing         │
│  • Batched writes               │  • Progress callbacks                 │
│           │                     │           │                           │
│           ▼                     │           ▼                           │
│  /mnt/nas/library.db            │  /media/USB/.quadrature/library.db    │
│  (WAL mode, rw)                 │  (stored on drive itself)             │
│           │                     │                                       │
│           │ NFS (read-only)     │                                       │
│           ▼                     │                                       │
│  Studio clients                 │                                       │
└─────────────────────────────────┴───────────────────────────────────────┘
```

## Filesystem Requirements

**fanotify requires a local filesystem with proper kernel support.** Network filesystems don't work.

| Filesystem | Supported | Notes                           |
| ---------- | --------- | ------------------------------- |
| ext4       | ✓         | Recommended for portable drives |
| XFS        | ✓         | Good for large files            |
| Btrfs      | ✓         | Snapshots, checksums            |
| ZFS        | ✓         | Enterprise NAS                  |
| NFS/CIFS   | ✗         | Read-only client access only    |
| exFAT/NTFS | ✗         | Rejected at mount detection     |

Portable drives **must** be ext4, XFS, Btrfs, or ZFS. The UI rejects other formats.

______________________________________________________________________

## Part 1: NAS Library

Indexer daemon runs on NAS with local filesystem access. Clients read database via NFS.

### fanotify Watcher

```c
// Watch entire mount point with single fd
int fan_fd = fanotify_init(FAN_CLASS_NOTIF | FAN_NONBLOCK, O_RDONLY);
fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
              FAN_CREATE | FAN_DELETE | FAN_MODIFY | FAN_MOVED_FROM | FAN_MOVED_TO,
              AT_FDCWD, "/mnt/broadcast/music");

// Event loop
struct fanotify_event_metadata buf[256];
while ((len = read(fan_fd, buf, sizeof(buf))) > 0) {
    // Process events, debounce, queue for indexing
}
```

### Indexer Components

- **Scanner**: Multi-threaded directory walker with delta detection
- **Writer**: Single-threaded batched transactions
- **Watcher**: fanotify with debounce (500ms)
- **Database**: SQLite WAL mode

### Configuration

```ini
# /etc/quadrature/indexer.conf
[daemon]
pid_file = /var/run/quadrature-indexer.pid
log_file = /var/log/quadrature/indexer.log

[database]
path = /mnt/broadcast/library/library.db
checkpoint_interval = 300

[scanner]
watch_paths = /mnt/broadcast/music
batch_size = 500
reconcile_interval = 24    # hours

[watcher]
debounce_ms = 500
```

### NFS Client Access

```c
// Client opens read-only
sqlite3_open_v2("file:///mnt/nas/library.db?mode=ro", &db, SQLITE_OPEN_READONLY, NULL);
```

Clients refresh queries on user action or periodic timer. No sync daemon needed.

______________________________________________________________________

## Part 2: Portable Drives

Indexed client-side when mounted. Database stored on drive for portability.

### Detection Flow

```
Drive Mounted
      │
      ▼
Check .quadrature/library.db exists?
      │
   ┌──┴──┐
   │     │
   ▼     ▼
EXISTS   NOT FOUND
   │        │
   ▼        ▼
Auto-index  Prompt user
(if enabled)  "Index this drive?"
   │        │
   ▼        ▼
Incremental  Full index
scan         (or ignore)
```

### Filesystem Validation

```c
static void on_mount_added(GVolumeMonitor* monitor, GMount* mount, gpointer data) {
    const char* fs_type = get_mount_fstype(mount);

    // Reject unsupported filesystems
    if (!is_fanotify_compatible(fs_type)) {
        show_toast(app, "Unsupported filesystem",
                   "Drive uses %s. Please format as ext4.", fs_type);
        return;
    }
    // ... proceed with indexing
}

static bool is_fanotify_compatible(const char* fs) {
    return strcmp(fs, "ext4") == 0 ||
           strcmp(fs, "xfs") == 0 ||
           strcmp(fs, "btrfs") == 0 ||
           strcmp(fs, "zfs") == 0;
}
```

### Drive Layout

```
/media/DJ_USB/
├── .quadrature/
│   └── library.db
├── Electronic/
│   └── Artist/Album/track.flac
└── Jazz/
```

### Incremental Scanning

Uses `file_state` table to detect changes efficiently.

```
For each audio file:
  1. Lookup by path in file_state
  2. Not found → NEW, extract metadata
  3. Found but (mtime|size|inode) changed → MODIFIED, re-extract
  4. Found and unchanged → SKIP

After scan:
  Mark files with last_seen < scan_start as DELETED
```

**Performance:**

- Full index 5k tracks: ~2-5 min
- Incremental (no changes): ~5-10 sec
- Incremental (50 new): ~15-30 sec

### Read-Only Fallback

If drive is write-protected:

```
~/.cache/quadrature/drives/{filesystem-uuid}.db
```

### Client Indexer API

```c
typedef struct {
    size_t files_scanned, files_total;
    size_t files_new, files_updated, files_unchanged, files_deleted, errors;
    const char* current_file;
} index_progress_t;

typedef void (*index_progress_cb)(const index_progress_t* progress, void* user_data);
typedef void (*index_complete_cb)(quadrature_result_t result, void* user_data);

quadrature_result_t library_index_drive(
    const char* mount_path,
    index_progress_cb progress_cb,
    index_complete_cb complete_cb,
    void* user_data,
    atomic_bool* cancel_flag
);
```

Threading: Background thread, progress via `g_idle_add()`, cooperative cancellation.

______________________________________________________________________

## Database Schema

SQLite with WAL mode. Minimal fields for performance.

```sql
CREATE TABLE artists (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL
);

CREATE TABLE albums (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    artist_id INTEGER REFERENCES artists(id),
    path TEXT NOT NULL,            -- relative to music base
    year INTEGER,
    UNIQUE(title, artist_id)
);

CREATE TABLE tracks (
    id INTEGER PRIMARY KEY,
    title TEXT NOT NULL,
    artist_id INTEGER REFERENCES artists(id),
    album_id INTEGER REFERENCES albums(id),
    path TEXT NOT NULL UNIQUE,
    duration_ms INTEGER NOT NULL,
    track_num INTEGER
);

CREATE VIRTUAL TABLE tracks_fts USING fts5(
    title, content='tracks', content_rowid='id'
);

CREATE INDEX idx_tracks_album ON tracks(album_id, track_num);

-- Delta detection
CREATE TABLE file_state (
    id INTEGER PRIMARY KEY,
    path TEXT UNIQUE NOT NULL,
    mtime INTEGER NOT NULL,
    size INTEGER NOT NULL,
    inode INTEGER NOT NULL,
    track_id INTEGER,
    last_seen INTEGER NOT NULL,
    state INTEGER DEFAULT 0         -- 0=active, 1=deleted
);
```

**Storage:** ~100 bytes/track, FTS5 adds ~50% overhead. 1M tracks ≈ 150MB.

______________________________________________________________________

## Album Art

Thumbnails pre-generated by indexer, full-size loaded on demand.

### Discovery Order

1. `art.jpg`
1. `cover.jpg`
1. `folder.jpg`
1. `album.jpg`
1. `front.jpg`
1. Embedded (FFmpeg extraction)

### Storage

```
/mnt/broadcast/art/
└── thumb/
    └── {album_id}.jpg    # 48x48, JPEG 85%
```

Full-size art loaded from `{music_base}/{albums.path}/art.jpg` at runtime.
