# M5StickC PLUS Presenter

An M5StickC PLUS turned into a Bluetooth slide clicker for **Google Slides on an
iPad**. It pairs as a BLE HID keyboard and sends arrow keys, so it works with
anything that takes keyboard input — Slides, Keynote, PowerPoint, a browser.

Held in **portrait with the big M5 button at the top**. The screen draws an arrow
pointing at each button, so the device explains itself:

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
| Power button, held ~1.5s | — | cancellable shutdown countdown |

It advertises as **"Slide Remote"** — that is the name you look for in the iPad's
Bluetooth list, and it is deliberately not the repo name. Renaming it forces a
re-pair on every host that has already bonded.

## Why BLE HID and not Bluetooth Classic

iPadOS will not pair with a Bluetooth Classic HID device from a hobby board. It
accepts BLE HID (HID-over-GATT) only. That single constraint drives the whole
library choice below.

## Hardware

An M5StickC PLUS. Nothing else — the ESP32-PICO-D4 already has BLE, and the
board already has the buttons, the screen and a battery.

This is the **PLUS**, not the PLUS2: 4MB flash, AXP192 power chip, 135x240
screen. The PLUS2 would need a different board definition and drops the AXP192,
so the shutdown countdown's register poke would not apply.

## Build and flash

Install [PlatformIO Core](https://platformio.org/install/cli), then:

    git clone https://github.com/F86Pilot/m5stickc-plus-presenter
    cd m5stickc-plus-presenter
    pio run -t upload

That is also the whole edit-and-flash loop: change `src/main.cpp`, run it again,
about 30 seconds. To watch the device's log:

    pio device monitor

Ctrl-C exits. You cannot flash while the monitor is open — it holds the serial
port.

## Pairing

Settings -> Bluetooth -> "Slide Remote" under Other Devices. The screen shows
PAIRED when the bond completes, and reconnects on its own after that.

If a host refuses to reconnect, forget the device there and pair again — a stale
bond is the usual cause.

## Customising

Button mappings are at the top of `src/main.cpp`:

    static const uint8_t KEY_NEXT = KEY_RIGHT;
    static const uint8_t KEY_PREV = KEY_LEFT;

Other useful keys: `KEY_PAGE_DOWN` / `KEY_PAGE_UP` (the other common presenter
mapping), `KEY_ESCAPE` (leave present mode). The on-screen legend in
`drawLegend()` is separate text and will not follow a changed keycode.

`SCREEN_ROTATION` is `2` (portrait, big button at top); `0` flips it 180 degrees.
`SHUTDOWN_SECONDS` is the countdown length.

## Things that look like bugs but are not

- **Nothing happens when I press a button.** A Bluetooth keyboard sends keys to
  whatever app is frontmost. Slides must be open and in present mode, and you may
  need to tap the slide once to give it focus.
- **The on-screen keyboard vanished.** iPadOS hides it whenever a hardware
  keyboard is connected. Forget the device to get it back.
- **The screen dims.** Deliberate — the battery is only 120mAh. Any press
  restores full brightness.
- **powerOff() seems to do nothing.** Test on battery. With USB power attached
  the AXP192 will not stay off.

## Don't

Things that were tried here and deliberately reverted. Re-adding them is the
easy mistake.

- **Don't add a blank-screen button.** `b` is Google Slides' black-screen
  shortcut on desktop and does nothing in Slides on iPadOS. The button and its
  `BLANK >` legend were removed again.
- **Don't repaint a text region with a band sized to the new text.** `PAIRED`
  (size 2) and `waiting to pair` (size 1) share one region; clearing only the
  smaller band left green pixels showing through. `drawBand()` takes the
  region's fixed height, not the text's.
- **Don't show the last command sent.** It was noise — the slide changing is the
  feedback. Presses still go to serial.
- **Don't try to start the shutdown countdown when the button goes down.** The
  AXP192 latches "a long press happened" in register 0x46; it does not expose a
  live button level. ~1.5s of lead-in is unavoidable.
- **Don't use `M5.BtnPWR.wasPressed()`.** For the AXP192 power key M5Unified only
  ever sets `clicked` or `hold`. `wasPressed()` compiles and never fires; use
  `wasClicked()`.

## Notes on the build

- `boards/m5stick-c-plus.json` is ours. PlatformIO only ships `m5stick-c`, whose
  variant name predates arduino-esp32 3.x (the build fails on `pins_arduino.h`)
  and whose `-DARDUINO_M5Stick_C` selects the original StickC's 80x160 panel
  instead of the PLUS's 135x240.
- Uses the [pioarduino](https://github.com/pioarduino/platform-espressif32)
  platform fork, because the BLE HID library needs arduino-esp32 3.x and the
  official platform is still on 2.0.x.
- [HijelHID_BLEKeyboard](https://github.com/HijelHub/HijelHID_BLEKeyboard) was
  chosen over the far more popular `T-vK/ESP32-BLE-Keyboard`, whose own README
  rates iOS as "not stable" and which has not been updated for core 3.x.
  HijelHID's README documents arrow keys as `KEY_RIGHT_ARROW`; the actual header
  defines `KEY_RIGHT`. Trust `src/BLEHIDKeys.h`.
- Animations render through a full-screen `M5Canvas` (~65KB heap) and push one
  finished frame, rather than erasing and redrawing on the panel. The buffer is
  freed once the UI settles into its Ready state.
- Startup writes AXP192 register 0x36 bits[1:0] to stretch the hardware
  power-off hold from ~6s to 10s, so the countdown is not cut off mid-run. The
  auto-shutdown bit is left enabled on purpose: a hung firmware can still be
  switched off by holding the button.
