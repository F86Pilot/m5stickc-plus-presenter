// Slide Remote - M5StickC PLUS as a BLE presentation clicker.
//
// Held in PORTRAIT with the big M5 button at the TOP. In that orientation the
// side button lands on the middle-left edge, so the screen draws an arrow
// pointing at each one:
//
//        ^  NEXT     big M5 button, top          -> Right Arrow
//   <  BACK          side button, middle-left    -> Left Arrow
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

// Screen is 135 x 240 in portrait.
static const int SCREEN_W = 135;

// Dynamic regions. Each is a fixed band that gets fully cleared before it is
// repainted -- see drawBand(). Everything else is a static legend drawn once.
static const int Y_STATUS  = 194;
static const int H_STATUS  = 18;
static const int Y_BATTERY = 218;
static const int H_BATTERY = 10;

static const uint8_t BRIGHT_ACTIVE = 100;
static const uint8_t BRIGHT_IDLE   = 15;
static const uint32_t IDLE_AFTER_MS = 30000;

static bool     lastPaired   = false;
static bool     dimmed       = false;
static uint32_t lastActivity = 0;
static uint32_t lastBattery  = 0;
static int32_t  lastBattPct  = -1;

// Clear a whole band, then centre one line of text in it.
//
// The band height is a property of the region, NOT of the text being drawn.
// Clearing only what the new text needs is what left green pixels of "PAIRED"
// showing through the smaller "waiting to pair".
static void drawBand(int y, int h, int dy, uint16_t colour, uint8_t size,
                     const char *text) {
  M5.Display.fillRect(0, y, SCREEN_W, h, TFT_BLACK);
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

// The permanent legend: one arrow pointing at each physical button.
static void drawLegend() {
  M5.Display.fillScreen(TFT_BLACK);

  // Up arrow -> big M5 button, just above the top edge of the screen.
  M5.Display.fillTriangle(67, 3, 50, 23, 84, 23, TFT_WHITE);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setTextDatum(top_center);
  M5.Display.drawString("NEXT", SCREEN_W / 2, 36);

  M5.Display.drawFastHLine(0, 66, SCREEN_W, TFT_DARKGREY);

  // Left arrow -> side button on the middle-left edge.
  M5.Display.fillTriangle(3, 120, 23, 102, 23, 138, TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(top_center);
  M5.Display.drawString("BACK", SCREEN_W / 2, 112);

  M5.Display.drawFastHLine(0, 170, SCREEN_W, TFT_DARKGREY);

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
  M5.Display.setTextDatum(top_center);
  M5.Display.drawString("Slide Remote", SCREEN_W / 2, 178);
}

static void markActive() {
  lastActivity = millis();
  if (dimmed) {
    M5.Display.setBrightness(BRIGHT_ACTIVE);
    dimmed = false;
  }
}

// Send a key if we are actually paired. When we are not, the status line
// already says so, so there is nothing more to put on screen.
static void sendKey(uint8_t key, const char *label) {
  markActive();
  if (!kb.isPaired()) {
    Serial.println("press ignored - not paired");
    return;
  }
  kb.tap(key);
  Serial.printf("sent %s\n", label);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(SCREEN_ROTATION);
  M5.Display.setBrightness(BRIGHT_ACTIVE);

  Serial.begin(115200);
  Serial.println("\nSlide Remote booting");

  drawLegend();

  kb.begin();
  Serial.println("BLE advertising as 'Slide Remote'");

  drawStatus(false);

  lastActivity = millis();
}

void loop() {
  M5.update();   // reads and debounces the buttons

  // Repaint the status line only when it changes, so the screen does not
  // flicker and we are not painting 100 times a second.
  bool paired = kb.isPaired();
  if (paired != lastPaired) {
    drawStatus(paired);
    Serial.println(paired ? "paired" : "disconnected");
    lastPaired = paired;
    markActive();
  }

  if (M5.BtnA.wasPressed()) sendKey(KEY_NEXT, "NEXT");
  if (M5.BtnB.wasPressed()) sendKey(KEY_PREV, "BACK");

  uint32_t now = millis();

  // Report battery to the host so it shows in the iPad's Bluetooth panel,
  // and keep the on-screen readout current.
  if (now - lastBattery > 30000 || lastBattPct < 0) {
    lastBattery = now;
    int32_t pct = M5.Power.getBatteryLevel();
    if (pct != lastBattPct) {
      lastBattPct = pct;
      kb.setBatteryLevel((uint8_t)pct);
      drawBattery(pct);
    }
  }

  if (!dimmed && now - lastActivity > IDLE_AFTER_MS) {
    M5.Display.setBrightness(BRIGHT_IDLE);
    dimmed = true;
  }

  delay(10);
}
