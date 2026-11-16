# Game Boy (example)

A Game Boy and Game Boy Color emulator as a C app: the pattern for heavy apps in
[docs/C_API.md "Heavy apps and emulators"](../../../../docs/C_API.md#heavy-apps-and-emulators),
with the measurements. It needs Solana OS 2.3.0 (`ui_frame`, `files`, `sys_tick`) and runs at
full speed only as the store's native code: interpreted it is far too slow to play.

## Using it

1. Install it from the App Store (an unlisted copy of your own is enough), and open it
   once: the badge makes its folder on the microSD card, `hh/files/gameboy/`.
2. Copy a `.gb` or `.gbc` game **you own** into that folder: with a card reader (the card
   is on the badge's back), or over USB with
   `lua recv gameboy files/<name> <bytes> <sha256>` on the console.
3. Open the app. With one game it starts it; with several it lists them.

Keys: the d-pad, SELECT = A, CANCEL = B, SELECT and CANCEL tapped together = START, held
together for a second = the menu (the Game Boy's SELECT button, the screen size 1x / 1.5x,
save, save and quit). HOME leaves (the game is saved). Battery saves go to
`hh/files/gameboy/save/<game>.sav`. There is no sound: the badge has no speaker.

No game, BIOS or boot ROM is part of the SDK, and none is needed: the core starts
games without a boot ROM. For a free game to try, *Libbet and the Magic Floor* by Damian
Yerrick (zlib licence) is at <https://github.com/pinobatch/libbet/releases>, and Matt
Currie's test pictures `dmg-acid2` and `cgb-acid2` (MIT) at
<https://github.com/mattcurrie/cgb-acid2/releases>.

## The core

`walnut/` is [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) 1.2.4 (upstream commit
`a42c186917516cc9c80515ceae1121600a071628`), the Game Boy Color fork of
[Peanut-GB](https://github.com/deltabeard/Peanut-GB), MIT licensed:
[walnut/license.txt](walnut/license.txt) (Walnut-CGB), with Peanut-GB's and SameBoy's MIT
notices in the header itself. Changes, each marked "Solana OS SDK":

- `walnut_cgb.h` is split in two at its line 3184 (`walnut_cgb_cpu.h` is the rest), because
  the App Store takes files of at most 256 KB and the header is 280 KB. The first part
  closes its two open conditionals at the split and the second opens them again.
- Upstream includes `version.all` behind `__has_include`, which the store refuses (an
  app's files are known in advance); nothing uses it, so the include is gone. An
  `__has_include("intrin.h")` on the MSVC-only path is `0`.

Nothing else differs from upstream. `main.c` is this SDK's (MIT, as Solana OS).
