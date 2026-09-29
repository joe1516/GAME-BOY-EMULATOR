# Game Boy Emulator

An original Game Boy (DMG) emulator written in C11 with SDL2. It emulates the Sharp LR35902 CPU, the memory bus with cartridge mappers, the timer, joypad, serial port and a scanline PPU. **Sound is not emulated.**

It is built as an educational project: the core is small, split by hardware component, and every part is commented with what the real hardware does.

## Features

- **CPU (LR35902):** all legal base opcodes and all 256 `CB`-prefixed opcodes, flags (including half-carry and `DAA`), interrupts with priority, the delayed effect of `EI`, `HALT` and the `HALT` bug, illegal-opcode lock-up detection. Starts in the post-boot-ROM state, so no boot ROM is needed.
- **Memory bus:** full 16-bit map (ROM, VRAM, cartridge RAM, WRAM + echo, OAM, I/O, HRAM, IE), OAM DMA.
- **Cartridges:** ROM-only, **MBC1**, **MBC3** (no real-time clock), **MBC5**. Battery-backed RAM is saved to `<rom>.sav` on exit, on reset and every ~10 seconds.
- **Timer:** `DIV`/`TIMA`/`TMA`/`TAC` driven by the 16-bit internal counter (falling-edge behaviour), timer interrupt.
- **PPU:** 160×144, line-by-line rendering of background, window and sprites (8×8 and 8×16, flipping, priority, 10-sprites-per-line limit), STAT/VBlank interrupts with STAT blocking, LCD on/off.
- **Frontend:** resizable window with integer scaling, frame pacing at 59.73 Hz, pause, fast-forward, reset, screenshots, drag-and-drop ROM loading.
- **Headless runner (`gbcli`):** runs a ROM without a window, prints serial output (used by test ROMs), saves screenshots and prints an instruction trace in [gameboy-doctor](https://github.com/robert/gameboy-doctor) format.

## Build

You need a C11 compiler, `make`, and SDL2 development files (SDL2 is only needed for the window; `gbcli` and the tests build without it).

| Platform | Setup |
| --- | --- |
| Windows (MSYS2 UCRT64 shell) | `pacman -S mingw-w64-ucrt-x86_64-gcc make mingw-w64-ucrt-x86_64-SDL2` |
| Ubuntu / Debian | `sudo apt install build-essential libsdl2-dev` |
| macOS | `brew install sdl2` |

```
make            # builds gbplay (window) and gbcli (headless)
make demo       # generates roms/demo.gb, a small homebrew demo ROM
make test       # runs the self-test suite
```

## Run

```
./gbplay roms/demo.gb            # the included homebrew demo (no game needed)
./gbplay path/to/your-game.gb    # a ROM you are legally entitled to use
```

On Windows the executables are `gbplay.exe` and `gbcli.exe`.

### Controls

| Game Boy | Keys |
| --- | --- |
| D-pad | Arrow keys or WASD |
| A / B | X / Z (or K / J) |
| Start | Enter (or Space) |
| Select | Backspace (or Right Shift) |
| Pause | P |
| Fast-forward | hold Tab |
| Reset | F2 |
| Screenshot | F12 (saved next to the ROM as `.bmp`) |
| Quit | Esc |

The demo ROM scrolls a striped background; the D-pad changes the scrolling.

## Testing

**Unit tests** (`make test`): 90 checks over CPU instructions and flags, interrupts, `EI` delay, `HALT`, the timer, joypad matrix, OAM DMA, memory map, MBC1/3/5 bank switching, battery saves, PPU background/scroll/sprites/priority/VBlank, and an integration test that runs the demo ROM and drives it with joypad input.

**Hardware test ROMs** (`tools/run_test_roms.sh`, needs `bash` and `curl`): downloads the freely distributed [Blargg](https://github.com/retrio/gb-test-roms) and [dmg-acid2](https://github.com/mattcurrie/dmg-acid2) ROMs and runs them.

Results on this emulator:

| Test | Result |
| --- | --- |
| Blargg `cpu_instrs` (all 11 sub-tests) | pass |
| Blargg `instr_timing` | pass |
| Blargg `halt_bug` | pass |
| `dmg-acid2` (PPU) | pixel-identical to the reference image |

Debugging with a trace:

```
./gbcli game.gb --trace 1000 --cycles 100000 > trace.txt
./gbcli game.gb --serial --frames 8000                      # prints test ROM output
./gbcli game.gb --frames 300 --screenshot shot.ppm
```

## Project layout

```
.
├── Makefile
├── src/
│   ├── gb.h             # all data structures and the public API
│   ├── cpu.c            # LR35902 decoding and execution
│   ├── bus.c            # memory map, I/O registers, OAM DMA
│   ├── cart.c           # ROM header parsing, MBC1/MBC3/MBC5, battery saves
│   ├── ppu.c            # PPU timing, STAT interrupts, scanline rendering
│   ├── gb.c             # reset/boot state, main loop, timer, joypad
│   ├── frontend_sdl.c   # window, input, frame pacing (gbplay)
│   └── cli.c            # headless runner (gbcli)
├── tests/selftest.c     # unit + integration tests
├── tools/
│   ├── make_demo_rom.c  # generates the homebrew demo ROM
│   └── run_test_roms.sh # downloads and runs Blargg / dmg-acid2
├── roms/                # demo.gb is generated here
└── archive/gba-experiment/   # an earlier ARM7TDMI (GBA) experiment, not used
```

## How it fits together

`gb_step()` executes one instruction (or takes one interrupt) and returns the T-cycles it used; the timer and PPU are then advanced by that many cycles. `gb_run_frame()` repeats this until the PPU reaches VBlank. The bus (`gb_read` / `gb_write`) is the only way the CPU touches memory, which is where the mappers, I/O registers and DMA hook in.

## Known limitations

- **No sound** (APU).
- **Timing is per instruction**, not per memory access. The PPU is scanline-based rather than a cycle-accurate pixel FIFO, so games relying on mid-scanline effects may glitch. VRAM/OAM access is not blocked while the PPU is using it. OAM DMA completes instantly.
- **Not supported:** MBC2, MBC3 real-time clock, Game Boy Color, Super Game Boy, link cable, rumble.
- Verified with the test ROMs above and unit tests. Commercial games were not tested during development.

## References

- [Pan Docs](https://gbdev.io/pandocs/), the Game Boy technical reference
- [gbdev.io](https://gbdev.io/), the community hub and toolchain list
- [Blargg's test ROMs](https://github.com/retrio/gb-test-roms) and [dmg-acid2](https://github.com/mattcurrie/dmg-acid2)
- [gameboy-doctor](https://github.com/robert/gameboy-doctor) for trace comparison

## License

MIT, see `LICENSE`. Game Boy is a trademark of Nintendo; no Nintendo code or ROMs are included.
