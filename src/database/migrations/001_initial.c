/**
 * 001_initial.c — Baseline schema for Quadrature music library.
 *
 * Creates all core tables, indexes, FTS5 virtual tables, and BM25 rank config.
 * Applied to fresh databases as version 1.
 */

#include "../internal.h"
#include <glib.h>

quadrature_result_t
db_migration_001_initial(sqlite3 *db)
{
    char *err = NULL;
    int rc = sqlite3_exec(
        db,
        /* Artists */
        "CREATE TABLE IF NOT EXISTS artists ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL UNIQUE COLLATE NOCASE,"
        "  musicbrainz_id TEXT,"
        "  sort_name TEXT"
        ");"

        /* Albums */
        "CREATE TABLE IF NOT EXISTS albums ("
        "  id INTEGER PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  artist_id INTEGER REFERENCES artists(id),"
        "  path TEXT NOT NULL DEFAULT '',"
        "  year INTEGER,"
        "  is_compilation INTEGER DEFAULT 0,"
        "  last_updated_at INTEGER,"
        "  last_updated_size INTEGER,"
        "  musicbrainz_release_id TEXT,"
        "  musicbrainz_release_group_id TEXT,"
        "  mb_status INTEGER DEFAULT 0,"
        "  mb_resolved_at INTEGER"
        ");"

        /* Tracks */
        "CREATE TABLE IF NOT EXISTS tracks ("
        "  id INTEGER PRIMARY KEY,"
        "  title TEXT NOT NULL,"
        "  album_id INTEGER REFERENCES albums(id),"
        "  path TEXT NOT NULL,"
        "  duration_ms INTEGER NOT NULL,"
        "  track_num INTEGER,"
        "  disc_num INTEGER NOT NULL DEFAULT 1,"
        "  mtime INTEGER NOT NULL DEFAULT 0,"
        "  year INTEGER DEFAULT 0,"
        "  genre TEXT,"
        "  artist_display TEXT,"
        "  UNIQUE(album_id, path)"
        ");"

        /* Multi-artist junction table (WITHOUT ROWID: composite PK, small rows) */
        "CREATE TABLE IF NOT EXISTS track_artists ("
        "  track_id INTEGER NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,"
        "  artist_id INTEGER NOT NULL REFERENCES artists(id),"
        "  position INTEGER NOT NULL DEFAULT 0,"
        "  join_phrase TEXT NOT NULL DEFAULT '',"
        "  PRIMARY KEY (track_id, artist_id)"
        ") WITHOUT ROWID;"

        /* Full-text search — standalone FTS5 */
        "CREATE VIRTUAL TABLE IF NOT EXISTS tracks_fts USING fts5(title, artist, album);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS artists_fts USING fts5(name);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS albums_fts USING fts5(title, artist);"

        /* Indexer errors */
        "CREATE TABLE IF NOT EXISTS indexer_errors ("
        "  id INTEGER PRIMARY KEY,"
        "  path TEXT NOT NULL,"
        "  message TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')),"
        "  scan_generation INTEGER NOT NULL DEFAULT 0"
        ");"

        /* Indexes — albums */
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_albums_path ON albums(path) WHERE path != '';"
        "CREATE INDEX IF NOT EXISTS idx_albums_artist_year_title ON albums(artist_id, year, title);"
        "CREATE INDEX IF NOT EXISTS idx_albums_mb_release ON albums(musicbrainz_release_id) WHERE "
        "musicbrainz_release_id IS NOT NULL;"
        "CREATE INDEX IF NOT EXISTS idx_albums_mb_status ON albums(mb_status);"
        "CREATE INDEX IF NOT EXISTS idx_albums_year_title ON albums(year, title COLLATE NOCASE);"

        /* Indexes — tracks */
        "CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id, disc_num, track_num);"
        "CREATE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path);"
        "CREATE INDEX IF NOT EXISTS idx_tracks_year ON tracks(year);"
        "CREATE INDEX IF NOT EXISTS idx_tracks_genre ON tracks(genre);"
        "CREATE INDEX IF NOT EXISTS idx_tracks_album_genre ON tracks(album_id, genre);"

        /* Indexes — artists */
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_artists_mbid ON artists(musicbrainz_id) WHERE "
        "musicbrainz_id IS NOT NULL;"
        "CREATE INDEX IF NOT EXISTS idx_artists_name ON artists(name COLLATE NOCASE);"

        /* Indexes — track_artists */
        "CREATE INDEX IF NOT EXISTS idx_track_artists_artist ON track_artists(artist_id);"
        "CREATE INDEX IF NOT EXISTS idx_track_artists_track ON track_artists(track_id, position, "
        "artist_id);"

        /* Indexes — errors */
        "CREATE INDEX IF NOT EXISTS idx_errors_path ON indexer_errors(path);"
        "CREATE INDEX IF NOT EXISTS idx_errors_generation ON indexer_errors(scan_generation);"

        /* FTS5 BM25 rank weights */
        "INSERT OR REPLACE INTO tracks_fts(tracks_fts, rank) VALUES('rank', 'bm25(10, 5, 1)');"
        "INSERT OR REPLACE INTO albums_fts(albums_fts, rank) VALUES('rank', 'bm25(5, 1)');",
        NULL,
        NULL,
        &err);

    if (rc != SQLITE_OK) {
        g_critical("Migration 001 failed: %s", err);
        sqlite3_free(err);
        return QUADRATURE_ERROR_INTERNAL;
    }
    return QUADRATURE_OK;
}
