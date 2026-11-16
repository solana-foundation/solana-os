# Themes: your badge at a glance, in a look you pick

The Themes app as it is in the App Store. It works on any badge with Solana OS 0.5.0 or
later and asks for no permissions. Twelve looks (Lumen, Pearl, Black pearl, Icon grid,
Grouped list, Dial, Control tiles, Terminal, E-paper, Bold blocks, Monster RPG,
Streaming), each a full screen showing the owner, the badge ID, whether the badge is
online, and the battery. LEFT and RIGHT change the look; the choice is kept in the
app's storage.

An app can only draw its own screen, so a look lasts while Themes is open: it does not
restyle the home screen or other apps. The app does not use the `theme` module
([LUA_API.md](../../LUA_API.md#theme-always-allowed)), so it runs on badges without it.

- `main.lua`, `app.json`: the app.
- `art/*.png`: one 320x218 background per look, with the layout already drawn on it
  (Lua images have no blending). The app draws the live text over it in the built-in
  fonts. On the card the pictures go under the app's `files/` directory.
