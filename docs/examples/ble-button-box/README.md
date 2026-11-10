# Button box: a Bluetooth gadget and the badge app that reads it

A button on any ESP32 with Bluetooth LE, and a badge app that shows its presses. Use it as
the starting point for anything you build for the badge: a buzzer for a quiz, a door
sensor, a dial, a box at your booth that visitors press.

```
ble-button-box/
  CMakeLists.txt, sdkconfig.defaults, main/   the gadget: an ESP-IDF project (NimBLE)
  app/main.lua, app/app.json                  the badge app (permissions "ble" and "led")
```

See [the Connectivity guide](../../CONNECTIVITY.md) for when Bluetooth is the right choice,
and [`ble` in the Lua API](../../LUA_API.md#ble-permission-ble-solana-os-100) for every call.

## What goes over the air

The box serves one service and advertises it, so the badge scans for that service only:

| | UUID | |
|---|---|---|
| Service | `ffe0` | in the advertisement; the name (`Button Box`) is in the scan response |
| Characteristic | `ffe1` | read and notify, 2 bytes: `1` while the button is held (`0` when up), then the presses since power-on, 0 to 255 and round again |

Each change of the button is pushed to the connected device if it subscribed. The box takes
one connection at a time and doesn't pair, like the badge: anyone in range can read it.

`ffe0` is a 16-bit UUID many hobby modules use. For a gadget of your own that other
people's apps shouldn't mistake for theirs, pick a random 128-bit UUID instead (the badge
then has room for 6 bytes of advertised data, not 20; LUA_API.md "Being found").

## Build and flash the box

Any ESP32 with Bluetooth LE: the ESP32, ESP32-S3, C2, C3, C5, C6 or H2 (the ESP32-S2 has
no Bluetooth). It builds with ESP-IDF v5.5 and v6.0; it was checked with v5.5.4 for the
`esp32` and `esp32c3`, and v6.0.1 for the `esp32c5`.

```sh
cd ble-button-box
idf.py set-target esp32c3          # your chip
idf.py build
idf.py -p <port> flash monitor     # e.g. /dev/cu.usbmodem1101 or COM5
```

The button is the board's **BOOT** button by default, so a dev board needs no wiring:
GPIO0 on the ESP32 and ESP32-S3, GPIO9 on the C2, C3, C6 and H2, GPIO28 on the C5. To use
another pin, wire a button between it and ground and set it in `idf.py menuconfig`
(*Button box > Button GPIO*); the name is there too. The monitor prints `advertising as
"Button Box"`, then `button down` / `button up` as you press.

`idf.py set-target` warns that `BTDM_CTRL_MODE_*` are unknown symbols on every chip but
the original ESP32: those lines only matter there (they keep its Classic Bluetooth off).

Before the badge, you can check the box with any phone app that browses Bluetooth LE
devices (nRF Connect, LightBlue): find *Button Box*, open service `FFE0`, read `FFE1` and
turn on notifications, then press the button.

## Run the badge app

The emulator has no radio to reach a gadget, so run the app on a real badge (Solana OS
1.0.0 or later): drop the `app/` folder on `/apps/submit` and use **Run on badge**, which
copies it over USB and shows the badge's console. The app scans for `ffe0`, connects to the
first box it finds, reads the value, subscribes, and shows each press (the LEDs light while
the button is held). Switch the box off and on again: the app says it lost the box and
connects again by itself. SELECT drops the link and looks again.

## Make it yours

- **More inputs.** Add characteristics to the service (the badge reads and subscribes to
  any of them by UUID), or pack more state into `ffe1`: a value is up to 244 bytes.
- **Let the badge control it.** Add a characteristic with `BLE_GATT_CHR_F_WRITE` and handle
  `BLE_GATT_ACCESS_OP_WRITE_CHR` in its access callback; the app calls
  `link:write("ffe0", "<uuid>", data)` (with a callback when it must know it arrived).
- **Several boxes.** The badge holds one link at a time. To hear several gadgets at once,
  have them put their state in their advertisements (manufacturer data) instead, and scan
  without connecting: see [`../ble-nearby/`](../ble-nearby/).
- **Trust.** A press could come from anyone's box under the same name and UUID. If it
  should earn something, let your server decide (`http`), as the guide says.
