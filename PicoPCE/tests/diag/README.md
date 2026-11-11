# Recovery regression tests

These tests include the actual `pce_diag.cpp`, replacing only watchdog, SCB and
display hardware with stubs. They simulate resets and SRAM loss; they do not
reproduce the game or validate the physical panel/bootloader installed on a board.

From the repository root in WSL/Linux:

```sh
g++ -std=c++17 -O2 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all -I tests/diag/shim tests/diag/test_recovery.cpp -o build_hw16_diag/test_diag_recovery
./build_hw16_diag/test_diag_recovery
python3 tests/diag/check_layout.py build_hw16_diag/PicoPCE.elf.map ../pico-bootLoader/build_hw16/pico-bootLoader.elf.map
```

Covered: silent cold boot and intentional exit, pause/resume watchdog lifetime,
watchdog recovery, SRAM overwrite fallback, corruption rejection, one-shot report,
core1 HardFault without further core0 watchdog feeding, basic/extended exception
frames, frame bounds/alignment/stacking errors, and invalid VRAM DMA inputs.
Display strings must fit the GB_MODE viewport. SDK/loader scratch registers 4–7
must retain their values (apart from scratch[4], set by watchdog_enable itself).

The old generic NOLOAD report was at 0x20000110; the local HW16 loader has .data
starting at 0x20000120, overwriting the report on reset. R2 reserves 256 bytes
at 0x2007AF00, outside the app heap and the local loader's data, BSS and stacks.
Watchdog scratch 0–3 retain a versioned stage/PC/LR backup with an integrity check.
If full SRAM data is lost, the screen explicitly labels the partial report and
does not display invented zeros as unavailable fault registers.

R2 had an incorrect assumption in both the decoder and its old FP test: it
skipped 18 words before reading PC/LR. R3's regression fixture follows Arm
Cortex-M33 DUG Figure 2-6: integer registers come first even with FP storage.
The new fixture failed on the R2 code before the decoder was corrected. It
also places the reported misleading LR/PC values in the FP slots to catch a
repeat of that error. See `docs/RONDO_BASE_COMPARISON.md` for the primary source.

`PicoPCE_hw16_diag_r3.uf2`, its matching ELF/map and SHA256 hashes are generated
with `$env:PCE_DIAGNOSTICS='1'; .\build_core2350b.ps1` from PowerShell.

Automatic recovery is also enabled in the normal HW15/HW16 firmware builds.
Run `build_core2350b.ps1` without diagnostic environment variables for the
main `PicoPCE_hw16.uf2`; the script saves the matching ELF/map/hashes as well.
