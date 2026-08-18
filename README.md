# Slide Remote

M5StickC PLUS as a Bluetooth presentation clicker for Google Slides on an iPad.

Held in **portrait with the big M5 button at the top**. The screen draws an arrow
pointing at each button so it explains itself:

```
        ^
      NEXT          big M5 button, top edge
   ---------

 <  BACK            side button, middle-left

   ---------
   Slide Remote
     PAIRED
       87%
```

| Button | Sends | Does |
|---|---|---|
| Big M5 button, top | Right Arrow | next slide |
| Side button, middle-left | Left Arrow | previous slide |
| Power button | — | power only (hold ~6s off, 2s on) |

## The edit-and-flash loop

Change `src/main.cpp`, then run one command:

    pio run -t upload

That compiles and flashes in about 30 seconds. To watch the device's own log output:

    pio device monitor

Ctrl-C exits the monitor. You cannot flash while the monitor is open — it holds the
serial port. Quit it first.

## Remapping the buttons

Top of `src/main.cpp`:

    static const uint8_t KEY_NEXT = KEY_RIGHT;
    static const uint8_t KEY_PREV = KEY_LEFT;

Other useful keys: `KEY_PAGE_DOWN` / `KEY_PAGE_UP` (the other common presenter
mapping), `KEY_ESCAPE` (leave present mode).

If you change a mapping, remember the on-screen legend in `drawLegend()` is separate
text — the screen will not update itself to match a new keycode.

## Screen orientation

`SCREEN_ROTATION` near the top of `src/main.cpp` is `2` (portrait, big button at
the top). Set it to `0` to flip 180 degrees.

## Pairing to the iPad

Settings -> Bluetooth -> "Slide Remote" under Other Devices. The screen shows
PAIRED when the bond completes. After the first pairing it reconnects on its own.

If the iPad refuses to reconnect, forget the device in Bluetooth settings and pair
again — a stale bond on the host is the usual cause.

## Things that look like bugs but are not

- **Nothing happens when I press a button.** A Bluetooth keyboard sends keys to
  whatever app is frontmost. Slides has to be open and in present mode, and you may
  need to tap the slide once to give it focus.
- **The on-screen keyboard vanished.** iPadOS hides it whenever a hardware keyboard
  is connected. Forget the device to get it back.
- **The screen dims.** Deliberate — the battery is only 120mAh. Any button press
  brings it back to full brightness.

## Don't

- **Don't add a blank-screen button.** Tried sending `b`, which is Google Slides'
  black-screen shortcut on desktop. It does nothing in Slides on iPadOS. The button
  and its `BLANK >` legend were removed again.
- **Don't repaint a text region with a band sized to the new text.** PAIRED (size 2)
  and "waiting to pair" (size 1) share one region; clearing only the smaller band
  left green pixels of the old text showing through. `drawBand()` now takes the
  region's fixed height, not the text's.
- **Don't show the last command sent at the bottom of the screen.** It was noise —
  the slide changing on the TV is the feedback. Presses are still logged to serial.

## Notes for future work

- The board definition in `boards/m5stick-c-plus.json` is ours. The platform only
  ships `m5stick-c`, whose variant name predates arduino-esp32 3.x (the build fails
  on `pins_arduino.h`) and whose `-DARDUINO_M5Stick_C` would select the original
  StickC's 80x160 screen instead of the PLUS's 135x240.
- `M5.BtnPWR` only ever reports `clicked` or `hold`, never `pressed`. If you ever
  wire it up, use `wasClicked()`; `wasPressed()` compiles but never fires.
- The HijelHID README documents arrow keys as `KEY_RIGHT_ARROW`. The actual header
  defines `KEY_RIGHT`. Trust `src/BLEHIDKeys.h`, not the README.
