# Regression: off-screen sprite corrupts the CD audio pointer

The R3 hardware capture resolved to `memcpy` at 0x100D6E86, called by
`cd_audio_generate_samples` at `cd.c:2316`. Its first store wrote to R0=0x000735A0,
matching BFAR. The source was valid PSRAM (R1=0x1129F02C).

The R3 linker layout put `cd_audio_buffer` at 0x2001B084 and the visible line
at 0x2001B0A4: the audio pointer was 32 bytes before the line. The physical
line has only 16 bytes of left scratch space. The renderer uses a virtual
framebuffer base for one-line rendering, so its full-frame bounds cannot
protect memory before the physical row on later scanlines.

A 32-pixel-wide sprite at x=-29 has a completely invisible 16-pixel cell.
The old code still rendered that cell and overwrote the high byte of the
adjacent audio pointer. The regression executes the actual renderer with that
layout, not a reimplementation. Before the fix it prints:

```
Audio pointer after off-left sprite: 000735A0 (expected 200735A0)
```

After the fix the pointer remains intact. An independent bitplane decoder
also checks all visible pixels for 119,808 cases covering x=-32..383,
16/32-pixel widths, 16/32/64-pixel heights, both flips, several scanlines,
transparent and packed opaque stores. Canary regions check both ends of the
physical scanline. The 16 right-hand scratch pixels are not visible output.

Build/run in WSL/Linux from the repository root:

```sh
gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare -ffunction-sections -fdata-sections -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=all -I tests/render/shim tests/render/test_sprite_bounds.c -Wl,--gc-sections -o build_hw16_diag/test_sprite_bounds
./build_hw16_diag/test_sprite_bounds
```

Alignment sanitization is disabled because the existing renderer intentionally
uses unaligned word stores supported on Cortex-M33. Address and other undefined
behavior checks remain enabled. The test does not run the disc image or replace
the required three passes through the cutscene and gameplay checks on hardware.

Build the fixed HW16 firmware, preserving the prior UF2 and R3 capture ELF:

```powershell
.\build_core2350b.ps1 -RondoFix
```

Output: `PicoPCE_hw16_rondo_fix.uf2`, with its own ELF/map and SHA256 hashes.
The existing R3 diagnostic stays enabled during games to capture another fault
if one remains. Cold boot, pause menus and normal exits do not show a report.
