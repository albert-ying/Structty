# Sixel Rendering Mode for Structty

## Summary

Add a `--sixel` flag that renders protein structures as pixel-level Sixel graphics in terminals that support it (iTerm2, foot, mlterm, etc.). The ASCII ncurses mode is preserved as the default.

## Architecture

Reuse the existing 3D projection pipeline. Replace the character-output stage with: RGBA pixel buffer → Sixel encoder → stdout.

```
Atoms → project() → RenderPoints → RGBA pixel buffer → SixelEncoder → \033Pq...data...\033\\
```

## New Components

### SixelEncoder (src/visualization/SixelEncoder.hpp/.cpp)

- Input: RGBA pixel buffer, width, height
- Builds 256-color palette from Palettes::ID2RGBA (direct mapping, no quantization needed)
- Encodes band-by-band (6 pixel rows per Sixel band)
- Outputs DCS sequence: `ESC P 0;1;q [palette definitions] [pixel data] ESC \`
- Handles transparent pixels (background = black)

### SixelScreen (src/visualization/SixelScreen.hpp/.cpp)

- Queries terminal pixel dimensions via ioctl(TIOCGWINSZ) → ws_xpixel, ws_ypixel
- Reserves bottom ~100px for the info panel (rendered as text below the Sixel image)
- Manages high-res pixel buffer (terminal pixel width × available pixel height)
- Projects atoms at pixel resolution (same math as Screen::project but into pixel buffer)
- Anti-aliased line drawing (Xiaolin Wu algorithm)
- Depth shading: alpha from Camera::get_alpha_from_depth(), composited over black
- Raw terminal I/O via termios (no ncurses dependency in this path)
- Non-blocking keyboard input via poll() + read()
- 30 FPS target

### Parameters changes

- Add `--sixel` boolean flag
- Add `get_sixel()` accessor

### main.cpp changes

- If `--sixel`: skip initscr(), create SixelScreen, use raw terminal loop
- Else: existing ncurses path (unchanged)

## Sixel Encoding Format

Each frame outputs:
```
\033[H              (cursor home)
\033Pq              (start Sixel)
#0;2;r;g;b          (palette entries, semicolon-separated RGB 0-100)
...
#color!count~char   (pixel data, bands of 6 rows)
\033\\              (end Sixel)
```

Band encoding: each character encodes 6 vertical pixels as (value + 63), where each bit represents one pixel row. Colors are palette-indexed.

## Rendering Pipeline Detail

1. Clear pixel buffer to black RGBA(0,0,0,255)
2. For each protein, for each chain, for each consecutive atom pair:
   - Perspective project to pixel coordinates (same FOV formula)
   - Draw anti-aliased line segment into pixel buffer with z-buffer test
   - Color from palette lookup, alpha-modulated by depth
3. Encode pixel buffer as Sixel
4. Write cursor-home + Sixel to stdout (single write for flicker-free update)
5. Draw text info panel below image using ANSI escape sequences

## Files

| File | Status | Purpose |
|------|--------|---------|
| src/visualization/SixelEncoder.hpp | New | Sixel encoding class |
| src/visualization/SixelEncoder.cpp | New | Sixel encoding implementation |
| src/visualization/SixelScreen.hpp | New | Sixel render loop class |
| src/visualization/SixelScreen.cpp | New | Projection + compositing + input |
| src/structure/Parameters.hpp | Modified | Add --sixel flag |
| src/structure/Parameters.cpp | Modified | Parse --sixel flag |
| main.cpp | Modified | Branch on --sixel |
| src/CMakeLists.txt | Modified | Add new source files |

No changes to existing Screen.cpp, Camera.cpp, or any other rendering code.
