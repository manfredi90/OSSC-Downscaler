Changelog
=========

v1.2
----

### New
- 1080p Wide (180p): the whole 16:9 picture letterboxed, 1848x1080 -> 308x180
  (6x6 pixel average; with line blending the 4 middle lines of each group of
  6), 240p and 480i output. "HD format" now offers Wide for 1080p too.
- 576p input (50Hz): 288p (310–314 lines, 15.5–15.7kHz) and 576i output,
  every source pixel sampled as for 480p, optional flicker filter in 576i.
- 576i -> 288p ("480i input" set to 240p) verified on hardware (PS2).

### Internal
- 6-sample horizontal box filter and groups of up to 6 input lines in the
  FPGA line buffer.


v1.1
----

### New
- 1080p input (60Hz), 240p and 480i output, 4 input lines per output line:
  - Full 16:9: the 1708x960 center window (~89% of the picture), squeezed to 4:3
  - Center 4:3: the 1280x960 center window
  - best with sources scaled x4 vertically (integer scaling)
- "480i input" menu option: 480i sources can be shown as 240p, with both
  fields on the same scanlines (no interlacing). Passthrough remains the
  default. PAL (576i -> 288p) is not tested yet.
- "480i filter" menu option (Off, 1–4): flicker filter for 480p and 640x480
  sources with 480i output. Each field line is mixed with the lines above and
  below (weights a, 1-2a, a; level 4 = 1/4, 1/2, 1/4).
- Component input (YPbPr) verified.

### Changed
- "720p format" renamed to "HD format" (applies to 720p and 1080p; Wide is
  720p only, 1080p uses Full 16:9).
- Output line rate limits narrowed to 14.8–16.0kHz; the hardware watchdog
  now trips at 16.4kHz (was 16.9kHz).
- The LCD shows `DS vX.Y 15kHz` at start.

### Fixed
- Wide (180p): the sampling phase was spread over the 4-sample group and
  sampled on the pixel edges, causing shimmer on motion.
- Line buffer timing: the read/write window is now checked at the first and
  the last row, needed when the input is written faster than it is read
  (1080p).

### Internal
- Capture logic pipelined for the 148.5MHz 1080p pixel clock (capture side
  constrained at 150MHz).


v1.0
----

First release.

- Inputs (VGA, ~60Hz): 1280x720p, 720x480p, 640x480p
- Outputs: 240p (260–264 lines, 15.6–15.9kHz) or 480i (525 lines), frame
  locked to the input
- 720p formats: Full 16:9 (squeezed), Center 4:3, Wide (180p letterbox)
- Full resolution sampling with an exact output clock ratio (smooth
  horizontal scrolling); 480p / 640x480 stored in full RGB
- Composite sync on HSYNC for SCART: Off (H+V), XOR, AND, Standard
  (equalizing pulses and serrated vsync)
- Line blending, line phase, adjustable width and position, manual 480p /
  640x480 input selection
- 15kHz-only output: computed modes, 15.73kHz test pattern fallback and an
  FPGA hardware watchdog
- Runs from the secondary firmware slot; own settings sector, independent of
  the official firmware and profiles
