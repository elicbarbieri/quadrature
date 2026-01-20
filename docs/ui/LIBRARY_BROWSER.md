# Library Management UI

Manages all library sources. Shows NAS status and portable drives.

```
┌──────────────────────────────────────────────────────────┐
│ Libraries                               [◐ 1 indexing]   │
├──────────────────────────────────────────────────────────┤
│ PRIMARY LIBRARY                                          │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ Studio Main                              ● Online    │ │
│ │ /mnt/nas/music                                       │ │
│ │ 45,231 tracks · Last scan: 2h ago        [Refresh]  │ │
│ └──────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────┤
│ CONNECTED DRIVES                                         │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ DJ Bob's Collection                    ◐ Indexing    │ │
│ │ /media/DJBOB_USB                                     │ │
│ │ ████████████████░░░░░░░░  62%                        │ │
│ │ 1,847 of 2,980 files                                 │ │
│ │ Processing: Electronic/Artist/track.flac   [Cancel] │ │
│ └──────────────────────────────────────────────────────┘ │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ Vinyl Rips                               ● Ready     │ │
│ │ /media/VINYL                                         │ │
│ │ 8,442 tracks · Just now (no changes)                 │ │
│ │                                    [Rescan] [Eject]  │ │
│ └──────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────┤
│ OFFLINE                                          [Hide]  │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ Summer Festival 2024                    ○ Offline    │ │
│ │ Last seen: 3 days ago · 12,847 tracks    [Forget]   │ │
│ └──────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

#### Library States

| State    | Badge             | Actions          |
| -------- | ----------------- | ---------------- |
| Online   | `● Online` green  | [Refresh]        |
| Checking | `◐ Checking` blue | -                |
| Indexing | `◐ Indexing` blue | [Cancel]         |
| Ready    | `● Ready` green   | [Rescan] [Eject] |
| Error    | `● Error` red     | [Retry] [Eject]  |
| Offline  | `○ Offline` gray  | [Forget]         |
| New      | `○ New` gray      | [Index] [Ignore] |

#### New Drive Prompt

```
┌──────────────────────────────────────────────────────┐
│ UNKNOWN_USB                                 ○ New    │
│ /media/UNKNOWN_USB                                   │
│                                                      │
│ This drive hasn't been indexed.                      │
│ Scan for music?              [Index Drive] [Ignore]  │
└──────────────────────────────────────────────────────┘
```

## Toast Notifications

Bottom-Left, auto-dismiss 5s (except prompts). Max 3 visible.

```
┌──────────────────────────────────────────┐
│ Drive connected                          │
│ Elis's Eclectica                         │
│ Checking for changes...           [View] │
└──────────────────────────────────────────┘
```
