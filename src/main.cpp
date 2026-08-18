// Slide Remote - M5StickC PLUS as a BLE presentation clicker.
//
// Held in PORTRAIT with the big M5 button at the TOP. In that orientation the
// side button lands on the middle-left edge, so the screen draws an arrow
// pointing at each one:
//
//        ^  NEXT     big M5 button, top          -> Right Arrow
//   <  BACK          side button, middle-left    -> Left Arrow
//
// The UI has three states:
//   Boot     - title fades up, then rises to make room
//   Pairing  - broadcast ripples while advertising
//   Ready    - the button legend, assembled by an animation on pair
//
// Build and flash:  pio run -t upload
// Watch the serial: pio device monitor

#include <M5Unified.h>
#include <HijelHID_BLEKeyboard.h>

// ---------------------------------------------------------------------------
// Change these to remap the buttons. Other useful keys: KEY_PAGE_DOWN /
// KEY_PAGE_UP (the other common presenter mapping), KEY_ESCAPE (leave
// present mode).
// ---------------------------------------------------------------------------
static const uint8_t KEY_NEXT = KEY_RIGHT;
static const uint8_t KEY_PREV = KEY_LEFT;

// Portrait with the big button at the top. Set to 0 to flip 180 degrees.
static const uint8_t SCREEN_ROTATION = 2;

HijelHID_BLEKeyboard kb("Slide Remote", "Preston", 100);

static const int SCREEN_W = 135;
static const int SCREEN_H = 240;

// Ready-mode layout. Each dynamic band is fully cleared before repaint -- the
// height belongs to the region, not to the text going into it, or taller old
// text shows through shorter new text.
static const int Y_STATUS  = 194;
static const int H_STATUS  = 18;
static const int Y_BATTERY = 218;
static const int H_BATTERY = 10;

// Pairing-mode ripple geometry.
static const int RIPPLE_CX = 67;
static const int RIPPLE_CY = 148;
static const int RIPPLE_MIN = 8;
static const int RIPPLE_MAX = 44;
static const int RIPPLE_RINGS = 3;

static const uint8_t BRIGHT_ACTIVE = 100;
static const uint8_t BRIGHT_IDLE   = 15;
static const uint32_t IDLE_AFTER_MS = 30000;

enum class UiMode { Pairing, Ready };
static UiMode   mode         = UiMode::Pairing;
static bool     dimmed       = false;
static uint32_t lastActivity = 0;
static uint32_t lastBattery  = 0;
static int32_t  lastBattPct  = -1;
static float    ripplePhase  = 0.0f;
static uint32_t lastRipple   = 0;

// Off-screen buffer for the animated modes. Drawing a whole frame here and
// pushing it once avoids the flicker of erase-then-redraw straight to the
// panel. Freed on entering Ready, which is the long-lived state.
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

// ---------------------------------------------------------------------------
// Ready mode: static legend plus two dynamic bands, drawn straight to the panel
// ---------------------------------------------------------------------------

static void drawBand(int y, int h, int dy, uint16_t colour, uint8_t size,
                     const char *text) {
  M5.Display.fillRect(0, y, SCREEN_W, h, TFT_BLACK);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextColor(colour, TFT_BLACK);
  M5.Display.setTextSize(size);
  M5.Display.setTextDatum(top_center);
  M5.Display.drawString(text, SCREEN_W / 2, y + dy);
}

static void drawStatus(bool paired) {
  if (paired) {
    drawBand(Y_STATUS, H_STATUS, 0, TFT_GREEN, 2, "PAIRED");
  } else {
    drawBand(Y_STATUS, H_STATUS, 5, TFT_ORANGE, 1, "waiting to pair");
  }
}

static void drawBattery(int32_t pct) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%ld%%", (long)pct);
  drawBand(Y_BATTERY, H_BATTERY, 0, TFT_DARKGREY, 1, buf);
}

// The permanent legend: one arrow pointing at each physical button. Drawn into
// whichever target is passed, so the same geometry serves both the animation
// (canvas) and the settled screen (panel).
static void paintLegend(LovyanGFX *g, float nextY, float nextFade,
                        float divTop, float backX, float backFade,
                        float divBottom, float titleFade) {
  // Up arrow -> big M5 button, just above the top edge of the screen.
  int ny = (int)nextY;
  g->fillTriangle(67, ny, 50, ny + 20, 84, ny + 20, TFT_WHITE);

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);
  if (nextFade > 0) {
    g->setTextColor(fade(255, 255, 255, nextFade));
    g->setTextSize(3);
    g->drawString("NEXT", SCREEN_W / 2, 36);
  }

  if (divTop > 0) {
    int half = (int)(divTop * SCREEN_W / 2);
    g->drawFastHLine(SCREEN_W / 2 - half, 66, half * 2, TFT_DARKGREY);
  }

  // Left arrow -> side button on the middle-left edge.
  int bx = (int)backX;
  g->fillTriangle(bx, 120, bx + 20, 102, bx + 20, 138, TFT_WHITE);

  if (backFade > 0) {
    g->setTextColor(fade(255, 255, 255, backFade));
    g->setTextSize(2);
    g->drawString("BACK", SCREEN_W / 2, 112);
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

static void drawLegendSettled() {
  M5.Display.fillScreen(TFT_BLACK);
  paintLegend(&M5.Display, 3, 1, 1, 3, 1, 1, 1);
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

  // Fade the backlight up with the title already on the panel.
  cv.fillSprite(TFT_BLACK);
  paintTitle(&cv, TITLE_REST, 1.0f);
  cv.pushSprite(0, 0);
  for (int b = 0; b <= BRIGHT_ACTIVE; b += 4) {
    M5.Display.setBrightness(b);
    delay(14);
  }
  delay(400);

  // Lift it to the top to clear space for the pairing ripples.
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
  cv.fillSprite(TFT_BLACK);
  paintTitle(&cv, 28, 1.0f);

  for (int i = 0; i < RIPPLE_RINGS; i++) {
    float p = phase + (float)i / RIPPLE_RINGS;
    if (p >= 1.0f) p -= 1.0f;
    int r = RIPPLE_MIN + (int)(p * (RIPPLE_MAX - RIPPLE_MIN));
    cv.drawCircle(RIPPLE_CX, RIPPLE_CY, r, fade(0, 200, 255, 1.0f - p));
  }
  cv.fillCircle(RIPPLE_CX, RIPPLE_CY, 4, fade(0, 200, 255, 1.0f));

  cv.setFont(&fonts::Font0);
  cv.setTextDatum(top_center);
  cv.setTextSize(1);
  cv.setTextColor(TFT_ORANGE);
  cv.drawString("waiting to pair", SCREEN_W / 2, 205);

  if (lastBattPct >= 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%ld%%", (long)lastBattPct);
    cv.setTextColor(TFT_DARKGREY);
    cv.drawString(buf, SCREEN_W / 2, 222);
  }
  cv.pushSprite(0, 0);
}

static void tickPairing() {
  if (!canvasOk) return;
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

static void enterReady() {
  pairSuccessAnimation();
  legendInAnimation();
  if (canvasOk) {
    cv.deleteSprite();          // Ready mode paints straight to the panel
    canvasOk = false;
  }
  drawLegendSettled();
  drawStatus(true);
  if (lastBattPct >= 0) drawBattery(lastBattPct);
  mode = UiMode::Ready;
}

static void enterPairing() {
  if (!canvasOk) canvasOk = (cv.createSprite(SCREEN_W, SCREEN_H) != nullptr);
  M5.Display.fillScreen(TFT_BLACK);
  ripplePhase = 0.0f;
  mode = UiMode::Pairing;
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

static const int SHUTDOWN_SECONDS = 6;
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

  LovyanGFX *g = canvasOk ? (LovyanGFX *)&cv : (LovyanGFX *)&M5.Display;
  if (canvasOk) { cv.fillSprite(TFT_BLACK); } else { M5.Display.fillScreen(TFT_BLACK); }

  g->setFont(&fonts::Font0);
  g->setTextDatum(top_center);
  g->setTextSize(1);
  g->setTextColor(TFT_ORANGE);
  g->drawString("POWERING DOWN", SCREEN_W / 2, 40);

  // A ring that empties as the countdown runs.
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

  if (canvasOk) cv.pushSprite(0, 0);
}

static void shutdownSequence() {
  bool createdHere = false;
  if (!canvasOk) {
    canvasOk = (cv.createSprite(SCREEN_W, SCREEN_H) != nullptr);
    createdHere = canvasOk;
  }

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
  if (createdHere) { cv.deleteSprite(); canvasOk = false; }
  if (mode == UiMode::Ready) {
    drawLegendSettled();
    drawStatus(true);
    if (lastBattPct >= 0) drawBattery(lastBattPct);
  } else {
    enterPairing();
  }
  suppressHoldUntil = millis() + 1500;   // do not re-trigger on the same hold
  markActive();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  extendHardwarePowerOffDelay();

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

  if (M5.BtnPWR.wasHold() && millis() > suppressHoldUntil) shutdownSequence();

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

  if (M5.BtnA.wasPressed()) sendKey(KEY_NEXT, "NEXT");
  if (M5.BtnB.wasPressed()) sendKey(KEY_PREV, "BACK");

  if (mode == UiMode::Pairing) tickPairing();

  uint32_t now = millis();

  // Report battery to the host so it shows in the iPad's Bluetooth panel,
  // and keep the on-screen readout current.
  if (now - lastBattery > 30000 || lastBattPct < 0) {
    lastBattery = now;
    int32_t pct = M5.Power.getBatteryLevel();
    if (pct != lastBattPct) {
      lastBattPct = pct;
      kb.setBatteryLevel((uint8_t)pct);
      if (mode == UiMode::Ready) drawBattery(pct);
    }
  }

  if (!dimmed && now - lastActivity > IDLE_AFTER_MS) {
    M5.Display.setBrightness(BRIGHT_IDLE);
    dimmed = true;
  }

  delay(10);
}
