Early experiment: an ARM7TDMI (Game Boy ADVANCE) CPU core.
The GBA is a different console from the original Game Boy, so this is kept
here for reference only and is not part of the DMG emulator in ../../src.
Build: gcc -o gba gameboy.c && gcc -o mk make_test_rom.c && ./mk && ./gba test.gba
