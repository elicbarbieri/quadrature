import urllib.request
import urllib.parse
import json
import time

BASE_URL = "https://musicbrainz.org/ws/2"
USER_AGENT = "quadrature/0.1 ( https://github.com/elicb/quadrature )"
RELEASE_MBID = "158f94bf-d77f-4055-afa4-8fc7e0b2a004"

request_count = 0

def mb_get(path, params=None):
    global request_count
    if params is None:
        params = {}
    params["fmt"] = "json"
    url = f"{BASE_URL}/{path}?{urllib.parse.urlencode(params)}"
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    request_count += 1
    return data

def fetch_release(mbid):
    return mb_get(f"release/{mbid}", {"inc": "recordings+artist-credits+labels+release-groups"})

def fetch_recording(mbid):
    time.sleep(1.1)
    return mb_get(f"recording/{mbid}", {"inc": "artist-credits"})

def parse_artist_credits(credits):
    result = []
    for i, entry in enumerate(credits):
        # Each entry is either an artist object or a join phrase string
        if isinstance(entry, dict) and "artist" in entry:
            artist = entry["artist"]
            credited_name = entry.get("name") or artist.get("name", "")
            join_phrase = entry.get("joinphrase", "")
            result.append({
                "position": i,
                "artist_mbid": artist.get("id", ""),
                "artist_name": artist.get("name", ""),
                "credited_name": credited_name,
                "sort_name": artist.get("sort-name", ""),
                "join_phrase": join_phrase,
            })
    return result


def main():
    print(f"Fetching release {RELEASE_MBID}...", flush=True)
    release_data = fetch_release(RELEASE_MBID)

    # Extract release-level info
    release_group = release_data.get("release-group", {})
    label_info = release_data.get("label-info", [])
    label_name = ""
    catalog_number = ""
    if label_info:
        first = label_info[0]
        label_name = (first.get("label") or {}).get("name", "")
        catalog_number = first.get("catalog-number") or ""

    release_out = {
        "mbid": release_data.get("id", ""),
        "title": release_data.get("title", ""),
        "date": release_data.get("date", ""),
        "country": release_data.get("country", ""),
        "label": label_name,
        "catalog_number": catalog_number,
        "release_group_mbid": release_group.get("id", ""),
        "status": release_data.get("status", ""),
    }

    tracks_out = []
    media_list = release_data.get("media", [])

    for disc_idx, medium in enumerate(media_list):
        disc_number = medium.get("position", disc_idx + 1)
        for track in medium.get("tracks", []):
            position = track.get("position", 0)
            title = track.get("title", "")
            recording = track.get("recording", {})
            recording_mbid = recording.get("id", "")
            duration_ms = track.get("length") or recording.get("length") or 0

            # Prefer recording-level artist credits over track-level
            rec_credits = recording.get("artist-credit", [])
            track_credits = track.get("artist-credit", [])

            # Use recording-level if available and non-empty
            credits_to_use = rec_credits if rec_credits else track_credits

            # Check if we need a follow-up: if recording-level credits are missing
            # and track credits only have 1 artist (might be missing feat. artists)
            needs_followup = False
            if not rec_credits:
                needs_followup = True

            if needs_followup and recording_mbid:
                print(f"  Follow-up request for recording {recording_mbid} (track: {title})", flush=True)
                time.sleep(1.1)
                rec_data = fetch_recording(recording_mbid)
                followup_credits = rec_data.get("artist-credit", [])
                if followup_credits:
                    credits_to_use = followup_credits

            artists = parse_artist_credits(credits_to_use)

            tracks_out.append({
                "position": position,
                "disc": disc_number,
                "title": title,
                "recording_mbid": recording_mbid,
                "duration_ms": duration_ms,
                "artists": artists,
            })

    output = {
        "release": release_out,
        "tracks": tracks_out,
    }

    print(json.dumps(output, indent=2, ensure_ascii=False))
    print(f"\n--- Total API requests made: {request_count} ---")

if __name__ == "__main__":
    main()
