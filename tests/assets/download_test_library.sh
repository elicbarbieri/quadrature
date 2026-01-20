#!/usr/bin/env bash
# Idempotent test library downloader - Public Domain music from Musopen
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIBRARY_DIR="$SCRIPT_DIR/library"

# Idempotent check: exit early if library exists with expected files
if [ -d "$LIBRARY_DIR" ]; then
    count=$(find "$LIBRARY_DIR" -type f \( -name "*.flac" -o -name "*.mp3" -o -name "*.ogg" -o -name "*.wav" \) 2>/dev/null | wc -l)
    if [ "$count" -ge 15 ]; then
        echo "Test library already exists at $LIBRARY_DIR ($count audio files)"
        exit 0
    fi
fi

command -v curl >/dev/null || { echo "curl required"; exit 1; }
command -v ffmpeg >/dev/null || { echo "ffmpeg required"; exit 1; }

BASE="https://archive.org/download/MusopenCollectionAsFlac"

# Track definitions: url_suffix|local_path
TRACKS=(
    # Bach Goldberg Variations (FLAC)
    "Bach_GoldbergVariations/JohannSebastianBach-01-GoldbergVariationsBwv.988-Aria.flac|Johann Sebastian Bach/Goldberg Variations/01 - Aria.flac"
    "Bach_GoldbergVariations/JohannSebastianBach-02-GoldbergVariationsBwv.988-Variation1.flac|Johann Sebastian Bach/Goldberg Variations/02 - Variation 1.flac"
    "Bach_GoldbergVariations/JohannSebastianBach-03-GoldbergVariationsBwv.988-Variation2.flac|Johann Sebastian Bach/Goldberg Variations/03 - Variation 2.flac"
    "Bach_GoldbergVariations/JohannSebastianBach-04-GoldbergVariationsBwv.988-Variation3.CanonOnTheUnison.flac|Johann Sebastian Bach/Goldberg Variations/04 - Variation 3.flac"
    # Beethoven String Quartet No. 6 (MP3)
    "Beethoven_StringQuartetNo.6inBFlatMajorOp.18/LudwigVanBeethoven-StringQuartetNo.6InBFlatMajorOp.18No.6-01-AllegroConBrio.mp3|Ludwig van Beethoven/String Quartet No. 6/01 - Allegro con brio.mp3"
    "Beethoven_StringQuartetNo.6inBFlatMajorOp.18/LudwigVanBeethoven-StringQuartetNo.6InBFlatMajorOp.18No.6-02-AdagioMaNonTroppo.mp3|Ludwig van Beethoven/String Quartet No. 6/02 - Adagio ma non troppo.mp3"
    "Beethoven_StringQuartetNo.6inBFlatMajorOp.18/LudwigVanBeethoven-StringQuartetNo.6InBFlatMajorOp.18No.6-03-ScherzoAllegro.mp3|Ludwig van Beethoven/String Quartet No. 6/03 - Scherzo Allegro.mp3"
    "Beethoven_StringQuartetNo.6inBFlatMajorOp.18/LudwigVanBeethoven-StringQuartetNo.6InBFlatMajorOp.18No.6-04-adagioLaMalinconia.mp3|Ludwig van Beethoven/String Quartet No. 6/04 - La Malinconia.mp3"
    # Borodin String Quartet No. 1 (OGG)
    "Borodin_StringQuartetNo.1inAMajor/AlexanderBorodin-StringQuartetNo.1InAMajor-01-Moderato-Allegro.ogg|Alexander Borodin/String Quartet No. 1/01 - Moderato-Allegro.ogg"
    "Borodin_StringQuartetNo.1inAMajor/AlexanderBorodin-StringQuartetNo.1InAMajor-02-AndanteConMoto.ogg|Alexander Borodin/String Quartet No. 1/02 - Andante con Moto.ogg"
    "Borodin_StringQuartetNo.1inAMajor/AlexanderBorodin-StringQuartetNo.1InAMajor-03-ScherzoPrestissimo.ogg|Alexander Borodin/String Quartet No. 1/03 - Scherzo Prestissimo.ogg"
    # Beethoven Symphony No. 3 Eroica (WAV from FLAC)
    "Beethoven_SymphonyNo.3Eroica/LudwigVanBeethoven-SymphonyNo.3InEFlatMajorEroicaOp.55-01-AllegroConBrio.flac|Ludwig van Beethoven/Symphony No. 3 Eroica/01 - Allegro con brio.wav"
    "Beethoven_SymphonyNo.3Eroica/LudwigVanBeethoven-SymphonyNo.3InEFlatMajorEroicaOp.55-02-MarciaFunebreAdagioAssai.flac|Ludwig van Beethoven/Symphony No. 3 Eroica/02 - Marcia funebre.wav"
    "Beethoven_SymphonyNo.3Eroica/LudwigVanBeethoven-SymphonyNo.3InEFlatMajorEroicaOp.55-03-ScherzoAllegroVivace.flac|Ludwig van Beethoven/Symphony No. 3 Eroica/03 - Scherzo Allegro vivace.wav"
    "Beethoven_SymphonyNo.3Eroica/LudwigVanBeethoven-SymphonyNo.3InEFlatMajorEroicaOp.55-04-FinaleAllegroMolto.flac|Ludwig van Beethoven/Symphony No. 3 Eroica/04 - Finale Allegro molto.wav"
)

echo "Downloading test library..."
mkdir -p "$LIBRARY_DIR"

for entry in "${TRACKS[@]}"; do
    url_suffix="${entry%%|*}"
    local_path="${entry##*|}"
    output="$LIBRARY_DIR/$local_path"
    mkdir -p "$(dirname "$output")"

    if [[ "$local_path" == *.wav ]]; then
        # Pipe FLAC through ffmpeg to produce WAV
        curl -fsSL "$BASE/$url_suffix" | ffmpeg -y -loglevel error -i pipe:0 "$output"
    else
        curl -fsSL --max-time 120 -o "$output" "$BASE/$url_suffix"
    fi
    echo "  $(basename "$output")"
done

# Cover art (colored images)
COVERS=("Johann Sebastian Bach/Goldberg Variations|darkgreen" "Ludwig van Beethoven/String Quartet No. 6|navy"
        "Ludwig van Beethoven/Symphony No. 3 Eroica|maroon" "Alexander Borodin/String Quartet No. 1|darkgoldenrod")
for entry in "${COVERS[@]}"; do
    dir="${entry%%|*}"; color="${entry##*|}"
    ffmpeg -y -loglevel error -f lavfi -i "color=c=$color:s=300x300:d=1" -frames:v 1 "$LIBRARY_DIR/$dir/cover.jpg"
done

echo "Done: $(find "$LIBRARY_DIR" -name "*.flac" -o -name "*.mp3" -o -name "*.ogg" -o -name "*.wav" | wc -l) tracks, 4 covers"
