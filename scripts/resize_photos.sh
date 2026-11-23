#!/bin/bash
# Prepares photos for the web page: converts to JPEG, applies the camera
# orientation, resizes to at most 1600 px and removes all metadata
# (including the GPS position recorded by phones).
#
# Usage: scripts/resize_photos.sh <photo folder> [max size in px]
# Output: docs/images/<name in lowercase>.jpg (existing files are overwritten)
#
# Needs macOS (sips, for HEIC photos) and ImageMagick (brew install imagemagick).

set -euo pipefail

src="${1:?usage: $0 <photo folder> [max px]}"
max="${2:-1600}"
dest="$(cd "$(dirname "$0")/.." && pwd)/docs/images"
mkdir -p "$dest"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

shopt -s nullglob nocaseglob
count=0
for f in "$src"/*.{jpg,jpeg,png,heic}; do
  name="$(basename "${f%.*}" | tr '[:upper:] ' '[:lower:]-')"
  # sips reads HEIC and PNG; ImageMagick then orients, resizes and strips.
  sips -s format jpeg "$f" --out "$tmp/in.jpg" >/dev/null
  magick "$tmp/in.jpg" -auto-orient -resize "${max}x${max}>" -strip \
    -quality 82 "$dest/$name.jpg"
  printf '%-40s %s\n' "$name.jpg" "$(du -h "$dest/$name.jpg" | cut -f1)"
  count=$((count + 1))
done

echo "$count photo(s) written to $dest"
