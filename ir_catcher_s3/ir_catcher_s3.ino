/**
 * ============================================================
 *  IR Signal Catcher — Alat Peraga Demo
 *  ESP32-S3 + OLED SSD1306 0.96" + IR Receiver
 *
 *  Library: U8g2 (by olikraus), IRremoteESP8266
 *
 *  Pin:
 *    OLED SDA  → GPIO 8
 *    OLED SCL  → GPIO 9
 *    IR DATA   → GPIO 4
 *
 *  Jejak Zaidan — Production Studio
 * ============================================================
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>

// ── PIN ──────────────────────────────────────────────────────
#define PIN_IR   4
#define OLED_SDA 8
#define OLED_SCL 9

// ── OLED (U8g2 SSD1306 I2C 128x64) ──────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ── IR ───────────────────────────────────────────────────────
IRrecv irrecv(PIN_IR);
decode_results irResult;

#define IR_BUF 80

// Simpan decoded value IR
uint64_t irON_val=0,        irOFF_val=0;
uint64_t irTEMP_UP_val=0,   irTEMP_DOWN_val=0;
decode_type_t irON_type=UNKNOWN, irOFF_type=UNKNOWN;
decode_type_t irTEMP_UP_type=UNKNOWN, irTEMP_DOWN_type=UNKNOWN;

// ── STATE ────────────────────────────────────────────────────
enum State       { STATE_LEARN, STATE_STANDBY, STATE_DETECTED };
enum LearnStep   { LEARN_ON=0, LEARN_OFF, LEARN_TEMP_UP, LEARN_TEMP_DOWN, LEARN_DONE };
State     appState   = STATE_LEARN;
LearnStep learnStep  = LEARN_ON;

String        detectedName = "";
unsigned long detectedAt   = 0;
#define DETECTED_SHOW_MS 2500

uint8_t       animFrame = 0;
unsigned long lastAnim  = 0;
#define ANIM_MS 120

const char* learnNames[] = { "AC ON", "AC OFF", "TEMP UP", "TEMP DOWN" };

// ── OLED HELPERS ─────────────────────────────────────────────
void drawCentered(const char* text, int y) {
    int w = u8g2.getStrWidth(text);
    u8g2.drawStr((128 - w) / 2, y, text);
}

// ── SCREENS ──────────────────────────────────────────────────
void drawLearnScreen() {
    char stepStr[16];
    snprintf(stepStr, sizeof(stepStr), "Langkah %d/4:", (int)learnStep + 1);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "REKAM SINYAL IR");
    u8g2.drawHLine(0, 12, 128);
    u8g2.drawStr(0, 26, stepStr);
    u8g2.setFont(u8g2_font_10x20_tf);
    drawCentered(learnNames[learnStep], 50);
    u8g2.sendBuffer();
}

void drawLearnSuccess() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "REKAM SINYAL IR");
    u8g2.drawHLine(0, 12, 128);
    // Centang sederhana
    u8g2.drawStr(40, 36, "OK!");
    drawCentered("Tersimpan", 52);
    u8g2.sendBuffer();
    delay(700);
}

void drawLearnDone() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    drawCentered("Semua sinyal", 24);
    drawCentered("berhasil direkam!", 36);
    drawCentered("Masuk standby...", 52);
    u8g2.sendBuffer();
    delay(2000);
}

void drawStandbyScreen() {
    // Dots animasi
    uint8_t dot = (animFrame / 4) % 4;
    char dots[5] = "   ";
    for (int i = 0; i < (int)dot; i++) dots[i] = '.';

    // Gelombang sederhana: 3 busur dengan radius berbeda
    uint8_t r1 = 6  + (animFrame % 8);
    uint8_t r2 = 14 + (animFrame % 8);
    uint8_t r3 = 22 + (animFrame % 8);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "IR CATCHER");
    u8g2.drawHLine(0, 12, 128);
    // Sumber sinyal
    u8g2.drawDisc(24, 42, 3);
    // Busur
    if (r1 < 30) u8g2.drawCircle(24, 42, r1, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
    if (r2 < 30) u8g2.drawCircle(24, 42, r2, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
    if (r3 < 30) u8g2.drawCircle(24, 42, r3, U8G2_DRAW_UPPER_RIGHT | U8G2_DRAW_LOWER_RIGHT);
    // Label
    u8g2.drawStr(60, 36, "Menunggu");
    u8g2.drawStr(60, 48, "sinyal IR");
    u8g2.drawStr(60, 60, dots);
    u8g2.sendBuffer();
}

void drawDetectedScreen() {
    unsigned long elapsed = millis() - detectedAt;
    int barW = (int)map(elapsed, 0, DETECTED_SHOW_MS, 128, 0);
    barW = constrain(barW, 0, 128);

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, "TERDETEKSI:");
    u8g2.drawHLine(0, 12, 128);

    // Ikon
    if (detectedName == "AC ON") {
        u8g2.drawDisc(20, 38, 7);
        // Sinar matahari — 8 titik arah
        static const int8_t dx[] = { 0, 7, 10, 7,  0, -7,-10,-7};
        static const int8_t dy[] = {-10,-7,  0, 7, 10,  7,  0,-7};
        static const int8_t ex[] = { 0, 9, 13, 9,  0, -9,-13,-9};
        static const int8_t ey[] = {-13,-9,  0, 9, 13,  9,  0,-9};
        for (int i = 0; i < 8; i++)
            u8g2.drawLine(20+dx[i], 38+dy[i], 20+ex[i], 38+ey[i]);
    } else if (detectedName == "AC OFF") {
        u8g2.drawCircle(20, 38, 9);
        u8g2.drawLine(13, 31, 27, 45);
        u8g2.drawLine(13, 45, 27, 31);
    } else if (detectedName == "TEMP UP") {
        u8g2.drawTriangle(20,26, 12,38, 28,38);
        u8g2.drawBox(17, 38, 7, 10);
    } else if (detectedName == "TEMP DOWN") {
        u8g2.drawBox(17, 28, 7, 10);
        u8g2.drawTriangle(20,52, 12,40, 28,40);
    } else {
        u8g2.setFont(u8g2_font_10x20_tf);
        u8g2.drawStr(14, 48, "?");
        u8g2.setFont(u8g2_font_6x10_tf);
    }

    // Nama sinyal
    u8g2.setFont(u8g2_font_9x15_tf);
    drawCentered(detectedName.c_str(), 52);
    u8g2.setFont(u8g2_font_6x10_tf);

    // Progress bar
    u8g2.drawBox(0, 62, barW, 2);
    u8g2.sendBuffer();
}

// ── IR CAPTURE & COMPARE ─────────────────────────────────────
bool captureDecoded(uint64_t& val, decode_type_t& type) {
    if (!irrecv.decode(&irResult)) return false;
    irrecv.resume();
    if (irResult.decode_type == UNKNOWN) return false;
    val  = irResult.value;
    type = irResult.decode_type;
    return true;
}

String identifySignal(uint64_t val, decode_type_t type) {
    Serial.printf("[ID] val=0x%llX type=%d | ON=0x%llX OFF=0x%llX UP=0x%llX DN=0x%llX\n",
        val, (int)type, irON_val, irOFF_val, irTEMP_UP_val, irTEMP_DOWN_val);
    if (val == irON_val)        return "AC ON";
    if (val == irOFF_val)       return "AC OFF";
    if (val == irTEMP_UP_val)   return "TEMP UP";
    if (val == irTEMP_DOWN_val) return "TEMP DOWN";
    return "Tdk Dikenal";
}

// ── SETUP ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000); // tunggu Serial siap
    Serial.println("[IR Catcher] Boot...");

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000); // 400kHz agar tidak lambat

    u8g2.setBusClock(400000);
    u8g2.begin();
    u8g2.setFont(u8g2_font_6x10_tf);
    Serial.println("[IR Catcher] OLED OK");

    irrecv.enableIRIn();
    Serial.println("[IR Catcher] IR OK. Mulai rekam.");

    appState  = STATE_LEARN;
    learnStep = LEARN_ON;
    drawLearnScreen();
}

// ── LOOP ─────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    // Update frame animasi
    if (now - lastAnim >= ANIM_MS) {
        lastAnim = now;
        if (++animFrame > 255) animFrame = 0;
    }

    // ── LEARN ────────────────────────────────────────────────
    if (appState == STATE_LEARN) {
        if (learnStep == LEARN_DONE) {
            drawLearnDone();
            appState = STATE_STANDBY;
            Serial.println("[Standby] Menunggu sinyal...");
            return;
        }

        uint64_t val; decode_type_t type;
        if (captureDecoded(val, type)) {
            switch (learnStep) {
                case LEARN_ON:       irON_val       = val; irON_type       = type; Serial.printf("[LEARN] ON  val=0x%llX\n", val); break;
                case LEARN_OFF:      irOFF_val      = val; irOFF_type      = type; Serial.printf("[LEARN] OFF val=0x%llX\n", val); break;
                case LEARN_TEMP_UP:  irTEMP_UP_val  = val; irTEMP_UP_type  = type; Serial.printf("[LEARN] UP  val=0x%llX\n", val); break;
                case LEARN_TEMP_DOWN:irTEMP_DOWN_val= val; irTEMP_DOWN_type= type; Serial.printf("[LEARN] DN  val=0x%llX\n", val); break;
                default: break;
            }
            drawLearnSuccess();
            learnStep = (LearnStep)((int)learnStep + 1);
            if (learnStep < LEARN_DONE) drawLearnScreen();
        }
        return;
    }

    // ── DETECTED ─────────────────────────────────────────────
    if (appState == STATE_DETECTED) {
        drawDetectedScreen();
        if (now - detectedAt >= DETECTED_SHOW_MS) {
            appState = STATE_STANDBY;
        }
        return;
    }

    // ── STANDBY ──────────────────────────────────────────────
    if (appState == STATE_STANDBY) {
        drawStandbyScreen();

        uint64_t val; decode_type_t type;
        if (captureDecoded(val, type)) {
            detectedName = identifySignal(val, type);
            detectedAt   = now;
            appState     = STATE_DETECTED;
            Serial.printf("[DETECT] %s\n", detectedName.c_str());
        }
        return;
    }
}
