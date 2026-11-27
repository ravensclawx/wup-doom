# wup-doom

**Doom on the Wii U GamePad**, for [wup-homebrew](https://github.com/ravensclawx/wup-homebrew) 2: the full game at
the pad's own speed, with the original **OPL music** (the Sound Blaster sound) and sound effects. The shareware
episode comes with it.

Get it from the Homebrew Menu's **App Store** (+): it installs the game and `doom1.wad` into `/apps/doom/`. To play
the full game or Doom II, copy your own `doom.wad` or `doom2.wad` there over File Sharing (the app looks for
`doom1.wad`; rename yours to that for now).

## Controls

| | |
|---|---|
| Left stick | move (forward and back: analog), strafe |
| Right stick | turn (analog) |
| ZR | fire |
| A | use, and confirm in the menus |
| ZL | run |
| L / R | previous / next weapon |
| + | the menu |
| − | the map |
| B | back, in the menus |
| Y / X | yes / no |
| D-pad | the menus, and moving |
| Home | back to the Homebrew Menu |

The game saves its settings and saved games next to itself (`/apps/doom/`).

## How it's made

- **The engine:** [doomgeneric](https://github.com/ozkl/doomgeneric) (Chocolate Doom's code, made portable), with
  the pad's own video, system and input (`src/wup_main.c`, `src/wup_system.c`).
- **The picture:** Doom draws 320 x 200 in 8-bit colour, which is exactly what the pad's 8-bit screen takes: no
  colour conversion, the palette goes straight to the LCD. Shown 4:3, as on a CRT (640 x 480).
- **Memory:** Doom's zone takes the app's heap and also the pad's RGB screen, which sits unused while Doom draws to
  the 8-bit one, about 1.6 MB in all (`src/wup_zone.c`).
- **The music:** Chocolate Doom's OPL player (`src/i_oplmusic.c`: each song as MIDI, played with the WAD's own
  instruments, GENMIDI) on an emulated YM3812, [emu8950](https://github.com/digital-sound-antiques/emu8950), at the
  chip's own rate, in an audio thread with the sound effects (`src/wup_sound.c`).
- **Loading:** the WAD is read with plain `read` calls, one per lump (`src/wup_wadfile.c`).

## Build

With [wup-homebrew](https://github.com/ravensclawx/wup-homebrew) checked out next to this folder (its libwup fetches
doomgeneric, Chocolate Doom and emu8950):

```
python3 ../wup-homebrew/libwup/tools/wupbuild.py .     # -> build/apps/doom.wup
```

## Licence

GPL-2.0 ([LICENSE](LICENSE)), as doomgeneric and Chocolate Doom; emu8950 is MIT. `doom1.wad` is id Software's
shareware episode, which may be shared freely as it is.
