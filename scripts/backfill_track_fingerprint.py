#!/usr/bin/env python3
"""
One-off backfill: download and import track_fingerprint data into acoustid_db.

Run on the server:
  sudo -u postgres psql -d acoustid_db -c "CREATE TABLE IF NOT EXISTS track_fingerprint (
      id INTEGER PRIMARY KEY,
      track_id INTEGER REFERENCES track(id),
      fingerprint_id INTEGER NOT NULL,
      submission_count INTEGER DEFAULT 0
  );
  CREATE INDEX IF NOT EXISTS track_fp_fingerprint_idx ON track_fingerprint(fingerprint_id);
  CREATE INDEX IF NOT EXISTS track_fp_track_idx ON track_fingerprint(track_id);"

Then:
  python3 backfill_track_fingerprint.py /mnt/deepstor/acoustid-data 2011-08-19 2026-03-24
"""

import sys, gzip, json, os, psycopg2
from psycopg2.extras import execute_values
from datetime import datetime, timedelta
from pathlib import Path
from urllib.request import urlretrieve
from urllib.error import HTTPError

DB_BATCH = 50000
BASE_URL = "https://data.acoustid.org"

data_dir   = Path(sys.argv[1])
start_date = datetime.strptime(sys.argv[2], "%Y-%m-%d")
end_date   = datetime.strptime(sys.argv[3], "%Y-%m-%d")

conn = psycopg2.connect("dbname=acoustid_db user=postgres")
cur  = conn.cursor()
cur.execute("SET synchronous_commit=OFF")
cur.execute("SET maintenance_work_mem='2GB'")
cur.execute("SET session_replication_role='replica'")
conn.commit()

total_days = (end_date - start_date).days + 1
total_rows = 0
downloaded = 0
current = start_date

while current <= end_date:
    day = current.strftime("%Y-%m-%d")
    day_num = (current - start_date).days + 1
    year = current.strftime("%Y")
    month = current.strftime("%Y-%m")
    current += timedelta(days=1)

    filename = f"{day}-track_fingerprint-update.jsonl.gz"
    filepath = data_dir / filename

    # Download if missing
    if not filepath.exists():
        url = f"{BASE_URL}/{year}/{month}/{filename}"
        try:
            urlretrieve(url, filepath)
            downloaded += 1
        except HTTPError:
            continue  # Some dates may not have data

    if not filepath.exists():
        continue

    # Import
    rows, n = [], 0
    cols = ["id", "track_id", "fingerprint_id", "submission_count"]
    try:
        with gzip.open(filepath, 'rt') as f:
            for line in f:
                if not line.strip(): continue
                d = json.loads(line)
                rows.append((d['id'], d['track_id'], d['fingerprint_id'], d.get('submission_count', 0)))
                n += 1
                if n % DB_BATCH == 0:
                    execute_values(
                        cur,
                        "INSERT INTO track_fingerprint (id,track_id,fingerprint_id,submission_count) VALUES %s ON CONFLICT DO NOTHING",
                        rows
                    )
                    conn.commit()
                    rows = []
    except Exception as e:
        print(f"  Error processing {filename}: {e}", flush=True)
        continue

    if rows:
        execute_values(
            cur,
            "INSERT INTO track_fingerprint (id,track_id,fingerprint_id,submission_count) VALUES %s ON CONFLICT DO NOTHING",
            rows
        )
        conn.commit()

    total_rows += n
    if day_num % 100 == 0 or day_num == total_days:
        print(f"[{day_num}/{total_days}] {day}  rows_so_far={total_rows:,}  downloaded={downloaded}", flush=True)

cur.execute("SET session_replication_role='origin'")
cur.execute("ANALYZE track_fingerprint")
conn.commit()
cur.close()
conn.close()

print(f"\nDone. {total_rows:,} rows imported, {downloaded} files downloaded.")
