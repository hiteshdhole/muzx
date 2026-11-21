// MUZX LCD hub: lyrics, clock, PC status, captions on an I2C character LCD
// (Arduino Uno R3).
//
// Wiring:  LCD GND->GND, VCC->5V, SDA->A4, SCL->A5
//          Mode button: one leg to D2, the other leg to GND (no resistor needed)
// Library: "LiquidCrystal I2C" (Frank de Brabander) via Library Manager.
//
// PC -> Arduino, one message per '\n' at 115200 baud:
//   plain text            wrapped and flipped through in pages (lyrics)
//   row1 <0x01> row2      two fixed rows, no wrapping (clock, PC status, bars)
//   bytes 0x10..0x17      draw a bar glyph, 1/8 .. 8/8 high (spectrum, volume bar)
// Arduino -> PC:
//   "B\n"  short press = next mode
//   "L\n"  long press  = previous mode
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

const uint8_t LCD_ADDR = 0x27;   // try 0x3F if the screen stays blank
const uint8_t COLS = 16;         // 20 for a 20x4 screen
const uint8_t ROWS = 2;          // 4 for a 20x4 screen
const uint8_t BUTTON_PIN = 2;
const unsigned long PAGE_MS = 1800;   // time per page when a line needs several pages
const unsigned long LONG_PRESS_MS = 700;

LiquidCrystal_I2C lcd(LCD_ADDR, COLS, ROWS);

// Bytes 0x10..0x17 in a message are bar glyphs: 1/8 (0x10) up to full (0x17).
const uint8_t GLYPH_FIRST = 0x10;
const uint8_t GLYPH_LAST = 0x17;

inline bool isGlyph(uint8_t c) { return c >= GLYPH_FIRST && c <= GLYPH_LAST; }

// Writes one character; glyph codes become the custom characters 0..7.
inline void putChar(uint8_t c) {
  lcd.write(isGlyph(c) ? (uint8_t)(c - GLYPH_FIRST) : c);
}

void createBarGlyphs() {
  uint8_t g[8];
  for (uint8_t h = 1; h <= 8; h++) {            // h = height in pixel rows
    for (uint8_t row = 0; row < 8; row++)
      g[row] = (row >= 8 - h) ? 0x1F : 0x00;    // filled from the bottom up
    lcd.createChar(h - 1, g);
  }
}

const uint8_t MAXLEN = 120;
char text[MAXLEN + 1];
uint8_t len = 0;
uint8_t lineStart[16], lineLen[16], lineCount = 0, page = 0;
unsigned long lastPage = 0;

// Split text into rows of at most COLS characters, breaking at spaces.
void wrapText() {
  lineCount = 0;
  uint8_t pos = 0;
  while (pos < len && lineCount < 16) {
    while (pos < len && text[pos] == ' ') pos++;
    if (pos >= len) break;
    uint8_t end = pos + COLS;
    if (end >= len) end = len;
    else {
      uint8_t cut = end;
      while (cut > pos && text[cut] != ' ') cut--;   // last space inside the row
      if (cut > pos) end = cut;
    }
    uint8_t n = end - pos;
    while (n > 0 && text[pos + n - 1] == ' ') n--;
    lineStart[lineCount] = pos;
    lineLen[lineCount] = n;
    lineCount++;
    pos = end;
  }
}

void showPage() {
  lcd.clear();
  for (uint8_t r = 0; r < ROWS; r++) {
    uint8_t i = page * ROWS + r;
    if (i >= lineCount) break;
    uint8_t n = lineLen[i];
    lcd.setCursor((COLS - n) / 2, r);              // centred, like Spotify
    for (uint8_t k = 0; k < n; k++) putChar(text[lineStart[i] + k]);
  }
}

// Two fixed rows: every row is padded with spaces, so the screen is never
// cleared and the clock does not flicker.
void showRows(uint8_t sep) {
  for (uint8_t r = 0; r < ROWS; r++) {
    uint8_t from = 0, to = len;
    if (r == 0) to = sep;
    else if (r == 1) from = sep + 1;
    else { from = len; }
    lcd.setCursor(0, r);
    uint8_t col = 0;
    for (uint8_t i = from; i < to && col < COLS; i++, col++) putChar(text[i]);
    for (; col < COLS; col++) lcd.write(' ');
  }
}

uint32_t shownHash = 0;
bool shownValid = false;

// The PC re-sends the current frame every few seconds as a keep-alive. A frame
// that is already on the screen must not restart the page flipping.
uint32_t hashText() {
  uint32_t h = 2166136261UL;
  for (uint8_t i = 0; i < len; i++) {
    h ^= (uint8_t)text[i];
    h *= 16777619UL;
  }
  return h;
}

void newLine() {
  text[len] = 0;
  const uint32_t h = hashText();
  if (shownValid && h == shownHash)
    return;
  shownHash = h;
  shownValid = true;
  uint8_t sep = 255;
  for (uint8_t i = 0; i < len; i++)
    if (text[i] == 1) { sep = i; break; }
  if (sep != 255) {            // two fixed rows
    lineCount = 0;
    showRows(sep);
    return;
  }
  wrapText();
  page = 0;
  lastPage = millis();
  showPage();
}

// ---- mode button -----------------------------------------------------------
bool btnDown = false;
unsigned long btnSince = 0;

void checkButton() {
  const bool pressed = digitalRead(BUTTON_PIN) == LOW;
  const unsigned long now = millis();
  if (pressed && !btnDown && now - btnSince > 40) {
    btnDown = true;
    btnSince = now;
  } else if (!pressed && btnDown && now - btnSince > 40) {
    btnDown = false;
    Serial.print(now - btnSince >= LONG_PRESS_MS ? F("L\n") : F("B\n"));
    btnSince = now;
  }
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  lcd.init();
  lcd.backlight();
  createBarGlyphs();
  lcd.setCursor(0, 0);
  lcd.print(F("MUZX LCD hub"));
  lcd.setCursor(0, 1);
  lcd.print(F("waiting..."));
}

void loop() {
  checkButton();
  while (Serial.available()) {
    uint8_t c = Serial.read();
    if (c == '\n') { newLine(); len = 0; continue; }
    if (c == '\r') continue;
    if ((c >= 32 || c == 1 || isGlyph(c)) && len < MAXLEN) text[len++] = c;
  }
  uint8_t pages = (lineCount + ROWS - 1) / ROWS;
  if (pages > 1 && millis() - lastPage > PAGE_MS) {   // long line: flip through pages
    page = (page + 1) % pages;
    lastPage = millis();
    showPage();
  }
}
