# Changelog

## 1.0.0 (2026-10-10)

First public release. Source is MIT; binaries are GPL-2.0 (they link NetSurf).

- Browse archive.org: search, collections and item pages.
- Audio and old-time radio with playlists and background play.
- Video: MPEG-4, H.264 and Theora over HTTPS range requests.
- Books: page images or text mode, category search; images viewer (IIIF).
- Wayback Machine viewer (NetSurf): snapshots by year, calendar, forms, zoom.
- Wii Menu channel (optional forwarder WAD, title `00010001-UIAC`) with generated banner and icon.
- Wii Remote, GameCube controller and USB keyboard input; 4:3 and 16:9.

Verified on a Wii running System Menu 4.3U and in Dolphin. Not yet tested: vWii (Wii U) and
other System Menu regions.

Notes:
- Wayback Machine requests are rate limited by web.archive.org (HTTP 429); the app waits and
  retries, and the limit shows in the overlay.
