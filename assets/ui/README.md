# UI art

Pixel art for the app's chrome. The files are embedded with `bin2s` and decoded with libpng at
start-up (`src/gfx/image.c`).

| File | Use |
|---|---|
| `icons/*.png` | Home screen tiles: `movies audio books images wayback search settings about favourite collections` |
| `logo.png` | Header mark, Homebrew Channel icon and channel banner |
| `pointer.png` | The normal pointer (a glove pointing up); hotspot at the fingertip, see `src/app/art.c` |
| `pointer_tap_0..4.png` | The pointer over anything clickable: the index finger taps, looping |
| `pointer_grab.png` | The pointer while dragging a page with B held (a closed fist seen from the back) |
| `pointer_wait.png` | The busy clock's face; the app draws its hands on top, turning (`src/ui/ui.c`) |
