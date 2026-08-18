// Slide Remote - M5StickC PLUS as a BLE presentation clicker.
//
// Held in PORTRAIT with the big M5 button at the TOP. In that orientation the
// side button lands on the middle-left edge, so the screen draws an arrow
// pointing at each one:
//
//        ^  NEXT     big M5 button, top          -> Right Arrow
//   <  BACK          side button, middle-left    -> Left Arrow
//
// The UI has four states:
//   Boot     - title fades up, then rises to make room
//   Pairing  - broadcast ripples while advertising
//   Ready    - the button legend; each press nudges its arrow
//   BtOff    - Bluetooth switched off on purpose (double-tap power)
//
// Side (BACK) button:
//   tap         previous slide
//   hold ~1s    toggle Bluetooth off/on (frees the iPad's on-screen keyboard
//               while the remote sits on a charger)
// Power button:
//   hold ~1.5s  cancellable shutdown countdown
//
// Build and flash:  pio run -t upload
// Watch the serial: pio device monitor

#include <M5Unified.h>
#include <HijelHID_BLEKeyboard.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Change these to remap the buttons. Other useful keys: KEY_PAGE_DOWN /
// KEY_PAGE_UP (the other common presenter mapping), KEY_ESCAPE (leave
// present mode).
// ---------------------------------------------------------------------------
static const uint8_t KEY_NEXT = KEY_RIGHT;
static const uint8_t KEY_PREV = KEY_LEFT;

// Portrait with the big button at the top. Set to 0 to flip 180 degrees.
static const uint8_t SCREEN_ROTATION = 2;

// How far an arrow+label nudges when its button is pressed, in pixels.
static const int PRESS_BUMP_PX = 8;

// How long to hold the side (BACK) button to toggle Bluetooth, in ms. Well
// above a normal back-tap so a slightly deliberate press never toggles by
// accident. This lives on a front GPIO button on purpose: the power button
// routes through the AXP192, which conflates a firm tap with a hold and makes
// multi-tap gestures there unreliable.
static const uint32_t BT_HOLD_MS = 900;

HijelHID_BLEKeyboard kb("Slide Remote", "Preston", 100);

static const int SCREEN_W = 135;
static const int SCREEN_H = 240;

// Ready-mode text positions.
static const int Y_STATUS  = 194;
static const int Y_BATTERY = 218;

// Ripple geometry, shared by the pairing animation and the BT-off motif.
static const int RIPPLE_CX = 67;
static const int RIPPLE_CY = 148;
static const int RIPPLE_MIN = 8;
static const int RIPPLE_MAX = 44;
static const int RIPPLE_RINGS = 3;

static const uint8_t BRIGHT_ACTIVE = 100;
static const uint8_t BRIGHT_IDLE   = 15;
static const uint32_t IDLE_AFTER_MS = 30000;

enum class UiMode { Pairing, Ready, BtOff };
static UiMode   mode         = UiMode::Pairing;
static bool     btEnabled    = true;
static bool     dimmed       = false;
static uint32_t lastActivity = 0;
static uint32_t lastBattery  = 0;
static int32_t  lastBattPct  = -1;
static float    ripplePhase  = 0.0f;
static uint32_t lastRipple   = 0;

// Off-screen buffer. Every frame is drawn here and pushed once, so nothing
// flickers from erase-then-redraw. Allocated once and kept for the whole run.
static M5Canvas cv(&M5.Display);
static bool canvasOk = false;

// Scale a colour toward black. k of 0 is black, 1 is full brightness.
static uint16_t fade(uint8_t r, uint8_t g, uint8_t b, float k) {
  if (k < 0) k = 0;
  if (k > 1) k = 1;
  return M5.Display.color565((uint8_t)(r * k), (uint8_t)(g * k), (uint8_t)(b * k));
}

// Map t onto a 0..1 ramp that starts at `from` and finishes at `to`.
static float segment(float t, float from, float to) {
  if (t <= from) return 0.0f;
  if (t >= to)   return 1.0f;
  return (t - from) / (to - from);
}

static float easeOut(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }

// Whichever surface we are drawing to this frame.
static LovyanGFX *surface() {
  return canvasOk ? (LovyanGFX *)&cv : (LovyanGFX *)&M5.Display;
}
static void beginFrame() {
  if (canvasOk) cv.fillSprite(TFT_BLACK); else M5.Display.fillScreen(TFT_BLACK);
}
static void endFrame() {
  if (canvasOk) cv.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Legend: one arrow pointing at each physical button
//
// nextY / backX position the arrows during the assemble animation; the fades
// and divider widths let it build up piece by piece. nextDy / backDx nudge a
// whole group (arrow plus its label) for the per-press bump.
// ---------------------------------------------------------------------------

static void paintLegend(LovyanGFX *g, float nextY, float nextFade,
                        float divTop, float backX, float backFade,
                        float divBottom, float titleFade,
                        int nextDy = 0, int backDx = 0) {
  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);

  // Up arrow -> big M5 button, just above the top edge of the screen.
  int ny = (int)nextY + nextDy;
  g->fillTriangle(67, ny, 50, ny + 20, 84, ny + 20, TFT_WHITE);
  if (nextFade > 0) {
    g->setTextColor(fade(255, 255, 255, nextFade));
    g->setTextSize(3);
    g->drawString("NEXT", SCREEN_W / 2, 36 + nextDy);
  }

  if (divTop > 0) {
    int half = (int)(divTop * SCREEN_W / 2);
    g->drawFastHLine(SCREEN_W / 2 - half, 66, half * 2, TFT_DARKGREY);
  }

  // Left arrow -> side button on the middle-left edge.
  int bx = (int)backX + backDx;
  g->fillTriangle(bx, 120, bx + 20, 102, bx + 20, 138, TFT_WHITE);
  if (backFade > 0) {
    g->setTextColor(fade(255, 255, 255, backFade));
    g->setTextSize(2);
    g->drawString("BACK", SCREEN_W / 2 + backDx, 112);
  }

  if (divBottom > 0) {
    int half = (int)(divBottom * SCREEN_W / 2);
    g->drawFastHLine(SCREEN_W / 2 - half, 170, half * 2, TFT_DARKGREY);
  }

  if (titleFade > 0) {
    g->setTextColor(fade(0, 255, 255, titleFade));
    g->setTextSize(1);
    g->drawString("Slide Remote", SCREEN_W / 2, 178);
  }
}

// Full settled Ready frame (legend + status + battery), with an optional
// per-press bump applied to one group.
static void paintReadyInto(LovyanGFX *g, int nextDy, int backDx) {
  paintLegend(g, 3, 1, 1, 3, 1, 1, 1, nextDy, backDx);

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);
  g->setTextColor(TFT_GREEN);
  g->setTextSize(2);
  g->drawString("PAIRED", SCREEN_W / 2, Y_STATUS);

  if (lastBattPct >= 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld%%", (long)lastBattPct);
    g->setTextColor(TFT_DARKGREY);
    g->setTextSize(1);
    g->drawString(buf, SCREEN_W / 2, Y_BATTERY);
  }
}

static void renderReady(int nextDy = 0, int backDx = 0) {
  beginFrame();
  paintReadyInto(surface(), nextDy, backDx);
  endFrame();
}

// A short, subtle nudge: the pressed arrow and its label move, then spring
// back. NEXT drops down; BACK slides right. sinf gives a smooth out-and-back.
static void pressBump(bool isNext) {
  const int N = 10;
  for (int i = 0; i <= N; i++) {
    float phase = sinf((float)i / N * 3.14159f);   // 0 -> 1 -> 0
    int d = (int)(PRESS_BUMP_PX * phase);
    renderReady(isNext ? d : 0, isNext ? 0 : d);
    delay(12);
  }
  renderReady(0, 0);
}

// ---------------------------------------------------------------------------
// Boot: title fades up under a rising backlight, then rises to make room
// ---------------------------------------------------------------------------

static void paintTitle(LovyanGFX *g, int topY, float k) {
  g->setFont(&fonts::Orbitron_Light_24);
  g->setTextDatum(top_center);
  g->setTextColor(fade(0, 255, 255, k));
  g->drawString("SLIDE", SCREEN_W / 2, topY);
  g->drawString("REMOTE", SCREEN_W / 2, topY + 28);
}

static void bootAnimation() {
  const int TITLE_REST = 95;   // centred while the screen fades up
  const int TITLE_TOP  = 28;   // final home, above the ripples

  M5.Display.setBrightness(0);
  M5.Display.fillScreen(TFT_BLACK);

  if (!canvasOk) {              // no buffer: just show the title and move on
    paintTitle(&M5.Display, TITLE_TOP, 1.0f);
    M5.Display.setBrightness(BRIGHT_ACTIVE);
    return;
  }

  cv.fillSprite(TFT_BLACK);
  paintTitle(&cv, TITLE_REST, 1.0f);
  cv.pushSprite(0, 0);
  for (int b = 0; b <= BRIGHT_ACTIVE; b += 4) {
    M5.Display.setBrightness(b);
    delay(14);
  }
  delay(400);

  for (int i = 0; i <= 16; i++) {
    float e = easeOut(i / 16.0f);
    int y = TITLE_REST + (int)((TITLE_TOP - TITLE_REST) * e);
    cv.fillSprite(TFT_BLACK);
    paintTitle(&cv, y, 1.0f);
    cv.pushSprite(0, 0);
    delay(22);
  }
}

// ---------------------------------------------------------------------------
// Pairing: concentric ripples, because the radio really is broadcasting
// ---------------------------------------------------------------------------

static void paintPairingFrame(float phase) {
  beginFrame();
  paintTitle(surface(), 28, 1.0f);

  LovyanGFX *g = surface();
  for (int i = 0; i < RIPPLE_RINGS; i++) {
    float p = phase + (float)i / RIPPLE_RINGS;
    if (p >= 1.0f) p -= 1.0f;
    int r = RIPPLE_MIN + (int)(p * (RIPPLE_MAX - RIPPLE_MIN));
    g->drawCircle(RIPPLE_CX, RIPPLE_CY, r, fade(0, 200, 255, 1.0f - p));
  }
  g->fillCircle(RIPPLE_CX, RIPPLE_CY, 4, fade(0, 200, 255, 1.0f));

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);
  g->setTextSize(1);
  g->setTextColor(TFT_ORANGE);
  g->drawString("waiting to pair", SCREEN_W / 2, 205);

  if (lastBattPct >= 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld%%", (long)lastBattPct);
    g->setTextColor(TFT_DARKGREY);
    g->drawString(buf, SCREEN_W / 2, 222);
  }
  endFrame();
}

static void tickPairing() {
  uint32_t now = millis();
  if (now - lastRipple < 40) return;
  lastRipple = now;
  ripplePhase += 0.018f;
  if (ripplePhase >= 1.0f) ripplePhase -= 1.0f;
  paintPairingFrame(ripplePhase);
}

// The ripples snap inward and turn green: the broadcast was answered.
static void pairSuccessAnimation() {
  if (!canvasOk) return;
  for (int i = 12; i >= 0; i--) {
    float p = i / 12.0f;
    cv.fillSprite(TFT_BLACK);
    paintTitle(&cv, 28, 1.0f);
    int r = RIPPLE_MIN + (int)(p * (RIPPLE_MAX - RIPPLE_MIN));
    cv.drawCircle(RIPPLE_CX, RIPPLE_CY, r, fade(0, 255, 120, 1.0f - p * 0.5f));
    cv.fillCircle(RIPPLE_CX, RIPPLE_CY, 4 + (12 - i) / 2, fade(0, 255, 120, 1.0f));
    cv.pushSprite(0, 0);
    delay(22);
  }
  delay(120);
}

// The legend assembles itself: each arrow arrives, then its label, then the
// rules that separate them.
static void legendInAnimation() {
  if (!canvasOk) return;
  const int FRAMES = 26;
  for (int i = 0; i <= FRAMES; i++) {
    float t = (float)i / FRAMES;
    cv.fillSprite(TFT_BLACK);
    paintLegend(&cv,
                -25 + 28 * easeOut(segment(t, 0.00f, 0.40f)),  // NEXT arrow in
                segment(t, 0.20f, 0.50f),                      // NEXT label
                segment(t, 0.35f, 0.60f),                      // top rule
                -30 + 33 * easeOut(segment(t, 0.45f, 0.75f)),  // BACK arrow in
                segment(t, 0.60f, 0.85f),                      // BACK label
                segment(t, 0.70f, 0.95f),                      // bottom rule
                segment(t, 0.85f, 1.00f));                     // title
    cv.pushSprite(0, 0);
    delay(18);
  }
}

// ---------------------------------------------------------------------------
// BT off: switched off deliberately. The Bluetooth rune with a red cross-out,
// so it reads instantly as "the radio is off".
// ---------------------------------------------------------------------------

// Draw a line with a bit of weight by stamping it across a small square.
static void thickLine(LovyanGFX *g, int x0, int y0, int x1, int y1,
                      uint16_t colour, int weight) {
  int h = weight / 2;
  for (int dx = -h; dx <= h; dx++)
    for (int dy = -h; dy <= h; dy++)
      g->drawLine(x0 + dx, y0 + dy, x1 + dx, y1 + dy, colour);
}

// The Bluetooth mark: a vertical spine, two triangular flags on the right, and
// two diagonals crossing through the centre to the left knees.
static void drawBluetoothLogo(LovyanGFX *g, int cx, int cy, int hh, int hw,
                              uint16_t colour, int weight) {
  int top = cy - hh, bot = cy + hh;
  int q1 = cy - hh / 2, q3 = cy + hh / 2;
  int r = cx + hw, l = cx - hw;
  thickLine(g, cx, top, cx, bot, colour, weight);   // spine
  thickLine(g, cx, top, r, q1, colour, weight);     // top -> upper-right knee
  thickLine(g, cx, bot, r, q3, colour, weight);     // bottom -> lower-right knee
  thickLine(g, r, q1, l, q3, colour, weight);       // upper-right -> lower-left
  thickLine(g, r, q3, l, q1, colour, weight);       // lower-right -> upper-left
}

static void renderBtOff() {
  beginFrame();
  LovyanGFX *g = surface();

  const int CX = RIPPLE_CX, CY = 124;
  drawBluetoothLogo(g, CX, CY, 30, 17, fade(60, 130, 210, 1.0f), 2);

  // Red cross-out, drawn over the logo and past its corners.
  thickLine(g, CX - 26, CY - 34, CX + 26, CY + 34, TFT_RED, 3);

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);
  g->setTextSize(1);
  g->setTextColor(fade(150, 150, 150, 1.0f));
  g->drawString("BLUETOOTH OFF", SCREEN_W / 2, 178);
  g->setTextColor(TFT_DARKGREY);
  g->drawString("hold BACK button", SCREEN_W / 2, 200);
  g->drawString("to turn back on", SCREEN_W / 2, 213);
  endFrame();
}

// ---------------------------------------------------------------------------

static void enterReady() {
  pairSuccessAnimation();
  legendInAnimation();
  renderReady();
  mode = UiMode::Ready;
}

static void enterPairing() {
  beginFrame();
  endFrame();
  ripplePhase = 0.0f;
  mode = UiMode::Pairing;
}

static void enterBtOff() {
  mode = UiMode::BtOff;
  renderBtOff();
}

static void markActive() {
  lastActivity = millis();
  if (dimmed) {
    M5.Display.setBrightness(BRIGHT_ACTIVE);
    dimmed = false;
  }
}

// Send a key if we are actually paired. When we are not, the screen already
// says so, so there is nothing more to put on it.
static void sendKey(uint8_t key, const char *label) {
  markActive();
  if (!kb.isPaired()) {
    Serial.println("press ignored - not paired");
    return;
  }
  kb.tap(key);
  Serial.printf("sent %s\n", label);
}

// Hold the side (BACK) button to disconnect from Bluetooth without powering
// off. end() drops the host, so the iPad restores its on-screen keyboard;
// begin() re-advertises and the existing bond reconnects on its own.
static void toggleBt() {
  btEnabled = !btEnabled;
  markActive();
  if (btEnabled) {
    Serial.println("BT enabled");
    kb.begin();
    enterPairing();
  } else {
    Serial.println("BT disabled");
    kb.end();
    enterBtOff();
  }
}

// ---------------------------------------------------------------------------
// Shutdown: a countdown you can call off
//
// The AXP192 reports the power button as a latched event in register 0x46, not
// as a live button level, and only once its own long-press threshold (~1.5s)
// is reached. A countdown therefore cannot begin at the instant of the press.
// What it can do is take over from there: on the long-press event we run our
// own countdown and call powerOff() at the end, so the user may let go
// straight away and can call the whole thing off with any other button.
// ---------------------------------------------------------------------------

static const int SHUTDOWN_SECONDS = 5;
static uint32_t suppressHoldUntil = 0;

// Buy room to finish. Register 0x36 bits[1:0] are the hardware power-off hold
// time (00=4s 01=6s 10=8s 11=10s); stretching it to 10s stops the AXP192
// cutting power mid-countdown if the button stays held. The auto-shutdown bit
// is deliberately left alone, so a hung firmware can still be switched off.
static void extendHardwarePowerOffDelay() {
  uint8_t reg = M5.Power.Axp192.readRegister8(0x36);
  M5.Power.Axp192.writeRegister8(0x36, (uint8_t)(reg | 0x03));
}

static void paintShutdownFrame(int remaining, float frac) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", remaining);

  beginFrame();
  LovyanGFX *g = surface();

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);
  g->setTextSize(1);
  g->setTextColor(TFT_ORANGE);
  g->drawString("POWERING DOWN", SCREEN_W / 2, 40);

  if (frac > 0.01f) {
    g->drawArc(67, 130, 40, 44, -90, -90 + 360.0f * frac, fade(255, 140, 0, 1.0f));
  }

  g->setFont(&fonts::Orbitron_Light_32);
  g->setTextDatum(middle_center);
  g->setTextColor(TFT_WHITE);
  g->drawString(buf, 67, 130);

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);
  g->setTextColor(TFT_DARKGREY);
  g->drawString("any button cancels", SCREEN_W / 2, 200);
  endFrame();
}

static void restoreCurrentMode() {
  if (mode == UiMode::Ready)      renderReady();
  else if (mode == UiMode::BtOff) renderBtOff();
  else                            enterPairing();
}

static void shutdownSequence() {
  M5.Display.setBrightness(BRIGHT_ACTIVE);
  dimmed = false;
  Serial.println("shutdown countdown started");

  uint32_t start = millis();
  bool cancelled = false;

  while (true) {
    M5.update();
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() || M5.BtnPWR.wasClicked()) {
      cancelled = true;
      break;
    }
    uint32_t elapsed = millis() - start;
    if (elapsed >= (uint32_t)SHUTDOWN_SECONDS * 1000) break;

    int remaining = SHUTDOWN_SECONDS - (int)(elapsed / 1000);
    float frac = 1.0f - (float)elapsed / (SHUTDOWN_SECONDS * 1000.0f);
    paintShutdownFrame(remaining, frac);
    delay(30);
  }

  if (!cancelled) {
    Serial.println("powering off");
    M5.Power.powerOff();
    return;                       // not reached
  }

  Serial.println("shutdown cancelled");
  restoreCurrentMode();
  suppressHoldUntil = millis() + 1500;   // do not re-trigger on the same hold
  markActive();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  extendHardwarePowerOffDelay();
  M5.BtnB.setHoldThresh(BT_HOLD_MS);   // long-press BACK toggles Bluetooth

  M5.Display.setRotation(SCREEN_ROTATION);

  Serial.begin(115200);
  Serial.println("\nSlide Remote booting");

  canvasOk = (cv.createSprite(SCREEN_W, SCREEN_H) != nullptr);
  if (!canvasOk) Serial.println("no canvas: animations disabled");

  bootAnimation();

  kb.begin();
  Serial.println("BLE advertising as 'Slide Remote'");

  enterPairing();
  lastActivity = millis();
}

void loop() {
  M5.update();   // reads and debounces the buttons

  if (M5.BtnPWR.wasHold() && millis() > suppressHoldUntil) { shutdownSequence(); return; }

  // Hold the side button to toggle Bluetooth. Outside the btEnabled block so it
  // works while off, to turn it back on. A hold never also fires a click, so
  // this can't send a stray BACK.
  if (M5.BtnB.wasHold()) { toggleBt(); return; }

  if (btEnabled) {
    bool paired = kb.isPaired();
    if (paired && mode == UiMode::Pairing) {
      Serial.println("paired");
      enterReady();
      markActive();
    } else if (!paired && mode == UiMode::Ready) {
      Serial.println("disconnected");
      enterPairing();
      markActive();
    }

    if (M5.BtnA.wasPressed()) { sendKey(KEY_NEXT, "NEXT"); if (mode == UiMode::Ready) pressBump(true); }
    if (M5.BtnB.wasClicked()) { sendKey(KEY_PREV, "BACK"); if (mode == UiMode::Ready) pressBump(false); }

    if (mode == UiMode::Pairing) tickPairing();
  }

  uint32_t now = millis();

  // Report battery to the host so it shows in the iPad's Bluetooth panel,
  // and keep the on-screen readout current.
  if (now - lastBattery > 30000 || lastBattPct < 0) {
    lastBattery = now;
    int32_t pct = M5.Power.getBatteryLevel();
    if (pct != lastBattPct) {
      lastBattPct = pct;
      if (btEnabled) kb.setBatteryLevel((uint8_t)pct);
      if (mode == UiMode::Ready) renderReady();
    }
  }

  if (!dimmed && now - lastActivity > IDLE_AFTER_MS) {
    M5.Display.setBrightness(BRIGHT_IDLE);
    dimmed = true;
  }

  delay(10);
}
