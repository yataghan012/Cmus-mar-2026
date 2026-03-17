cat > README.md << 'EOF'
# cmus 2.12.0 — ffmpeg 6.0+ patch

This is [cmus](https://cmus.github.io/) 2.12.0 with a patched `ip/ffmpeg.c`
for compatibility with **ffmpeg 6.0 and later** (including ffmpeg 8.x).

## Problem

The original `ip/ffmpeg.c` uses `avcodec_close()`, which was removed in
ffmpeg 6.0, causing the ffmpeg plugin to silently fail to compile. This
means formats like **M4A, AAC, WMA, OPUS** would not play even though
ffmpeg was installed.

## Fix

`ip/ffmpeg.c` has been rewritten to target the modern ffmpeg API:
- Removed `avcodec_close()` and `av_register_all()`
- Uses `av_packet_alloc()` / `av_packet_free()`
- Uses the `ch_layout` API (`AVChannelLayout`)
- Uses `avcodec_send_packet()` / `avcodec_receive_frame()` throughout
- Minimum supported ffmpeg: **6.0**

## Building
```bash
sudo pacman -S ffmpeg   # Arch / Manjaro
# or
sudo apt install ffmpeg # Debian / Ubuntu

./configure
make
sudo make install
```

## Original project

https://github.com/cmus/cmus
EOF

git add README.md
git commit -m "docs: add README explaining ffmpeg 6.0+ patch"
git push