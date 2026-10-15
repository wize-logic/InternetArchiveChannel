# Changelog

## Unreleased

- **Hours-long videos play.** FFmpeg's sample index for a 3.5 h recording (~1M samples) needs ~60 MB,
  more than the Wii has, so such a video froze at 0:00 and never started. MP4s with a big `moov`
  (over 6 MB) are now indexed by `src/media/mp4idx.c` at ~12 bytes a sample (12.7 MB for that
  video) and fed straight to the H.264/AAC decoders. Smaller files still use FFmpeg's demuxer.
- **Ogg videos play at full frame rate.** The decoder took packets in file order, but Ogg files
  interleave loosely (video up to ~1 s ahead of its audio, or audio ~0.4 s ahead of its video), so
  the short frame queue overflowed (dropped frames, brief audio gaps) or video waited behind audio
  (late frames): about 25 of 30 fps on hardware. Each stream now reads ahead into its own packet
  queue (up to 4 MB) and is decoded when it needs data. A 1985 CBS recording now shows 29.97 fps
  with no drops in Dolphin; H.264/AAC MP4s lose no frames either. (Low counts on some Ogg files are
  the file itself: Theora stores a repeated frame as an empty packet.)
- If audio never arrives, the picture no longer stays frozen at 0:00: after 8 s of video with no
  audio output the player runs on the video clock.
- Of equally ranked video files, the lower-bitrate one is chosen; the item page warns above
  1500 kbit/s ("high bitrate, may buffer").
- The progress log now ends with the heap in use, and the player logs index size and failed
  allocations; a video frame that cannot be allocated is skipped and logged.
- "Reading the video index (N MB)..." shows while a big index loads.
- **Back is a button; B is for scrolling.** As in the Wii's Internet Channel, holding B and moving
  the pointer drags the results grid, book pages, text and Wayback pages, and B does nothing else
  (a quick tap of B used to mean "back", so the app had to guess). Every screen but home has a Back
  button at the top left; the Wayback page view's "<" goes back through history, then out; the
  viewer and video player have Close.
- **Better search.** Every word must be in the title or creator, and results are sorted by
  popularity, limited to things the channel can open. Before, "night of the living dead" listed
  sermons and "star trek" listed Gunsmoke. If no title matches, it searches all fields and says so.
- **Numbered pages.** Results come 30 to a page with "(-) Prev  Page N of M  Next (+)" at the
  bottom (the - and + buttons work too), up to archive.org's limit of 10,000 results. Before, more
  results loaded only when a card near the end was highlighted, and stopped at 600.
- **Collections** on the home screen: the archive's video, audio, text and image collections, most
  visited first (Community Audio, LibriVox, Live Music Archive, Feature Films...), each opening as a
  list, with a search box for finding one.
- **Back returns to the list as you left it.** Opening a collection from a list replaced that list,
  so Back showed the collection again; each list is now kept (page, scroll and highlighted card).
  A search inside a category opens over its list, and Back returns to the same page of it.
- A new page starts at its top, on its first card (it scrolled to wherever the highlight had been).
- The category search keyboard opens with the words last searched for.
- Searches no longer list the TV News, lending-library and stream-only collections (or those
  inside them, like "CNN (San Francisco)"): none of their items can be opened, so they always
  opened empty. "Television" in Collections now finds the playable Television collection first.
- Collections can open inside collections up to 12 screens deep (8 before; past it, a card did nothing).

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
