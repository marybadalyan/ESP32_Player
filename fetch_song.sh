#!/bin/bash
# Usage: ./fetch_song.sh <youtube_url> <bitrate> <img_width> <img_height>
# Example: ./fetch_song.sh "https://youtube.com/watch?v=..." 64K 128 48

set -e

URL="$1"
BITRATE="${2:-128K}"
IMG_W="${3:-128}"
IMG_H="${4:-48}"
DATA_DIR="$(dirname "$0")/data"
TMP_DIR="$(mktemp -d)"

if [ -z "$URL" ]; then
  echo "Usage: $0 <youtube_url> [bitrate] [img_width] [img_height]"
  echo "  bitrate defaults to 128K (e.g. 64K, 192K, 320K)"
  echo "  img_width/height default to 128x48"
  exit 1
fi

echo "Downloading audio at $BITRATE..."
yt-dlp -x --audio-format mp3 --audio-quality "$BITRATE" \
  --postprocessor-args "ffmpeg:-ar 44100" \
  --write-thumbnail --convert-thumbnails jpg \
  -o "$TMP_DIR/track.%(ext)s" \
  "$URL"

echo "Copying song.mp3 → data/"
cp "$TMP_DIR/track.mp3" "$DATA_DIR/song.mp3"

echo "Resizing thumbnail → data/art.jpg (${IMG_W}x${IMG_H})"
ffmpeg -y -i "$TMP_DIR/track.jpg" \
  -vf "scale=${IMG_W}:${IMG_H}:force_original_aspect_ratio=decrease" \
  "$DATA_DIR/art.jpg"

rm -rf "$TMP_DIR"
echo "Done. data/song.mp3 and data/art.jpg are ready."
echo "Run: pio run --target uploadfs && pio run --target upload"
