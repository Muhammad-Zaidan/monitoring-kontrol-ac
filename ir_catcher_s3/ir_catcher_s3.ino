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
#include <IRac.h>

// ── PIN ──────────────────────────────────────────────────────
#define PIN_IR   4   // (Bebas, GPIO4 bisa digunakan)
#define OLED_SDA 21
#define OLED_SCL 22

// ── OLED (U8g2 SSD1306 I2C 128x64) ──────────────────────────
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// ── IR ───────────────────────────────────────────────────────
// Gunakan ukuran buffer besar (1024) dan timeout 50ms (standar 15ms).
// Timeout 50ms sangat penting agar rentetan sinyal AC yang terputus 
// (misal: Header lalu Data) digabung menjadi SATU sinyal utuh.
IRrecv irrecv(PIN_IR, 1024, 50, true);
decode_results irResult;

#define IR_BUF 80

// ── STATE ────────────────────────────────────────────────────
enum State { STATE_STANDBY, STATE_DETECTED };
State appState = STATE_STANDBY;

String        detectedName = "";
String        detectedLine2 = "";
unsigned long detectedAt   = 0;
#define DETECTED_SHOW_MS 3000

uint8_t       animFrame = 0;
unsigned long lastAnim  = 0;
#define ANIM_MS 120

// Tracking status sebelumnya untuk mendeteksi perubahan
bool    prevPowerKnown = false;
bool    prevPower      = false;
int     prevTemp       = -1;

// ── OLED HELPERS ─────────────────────────────────────────────
void drawCentered(const char* text, int y) {
    int w = u8g2.getStrWidth(text);
    u8g2.drawStr((128 - w) / 2, y, text);
}

// ── SCREENS ──────────────────────────────────────────────────

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
    u8g2.clearBuffer();
    
    // Baris 1: Aksi (AC ON, TEMP UP, dll)
    u8g2.setFont(u8g2_font_8x13B_tf);
    drawCentered(detectedName.c_str(), 25);
    
    // Baris 2: Suhu
    u8g2.setFont(u8g2_font_9x15_tf);
    drawCentered(detectedLine2.c_str(), 50);

    // Progress bar
    unsigned long elapsed = millis() - detectedAt;
    int barW = (int)map(elapsed, 0, DETECTED_SHOW_MS, 128, 0);
    barW = constrain(barW, 0, 128);
    u8g2.drawBox(0, 62, barW, 2);
    u8g2.sendBuffer();
}

// ── IR FLUSH (MENGABAIKAN SISA SINYAL BERUNTUN DARI AC) ────────
void flushIR() {
    Serial.println("[DEBUG IR] Membersihkan sisa rentetan sinyal dari remote...");
    unsigned long lastSignal = millis();
    while (millis() - lastSignal < 600) {
        if (irrecv.decode(&irResult)) {
            lastSignal = millis();
            irrecv.resume();
        }
        delay(10);
    }
    irrecv.resume();
    Serial.println("[DEBUG IR] Saluran IR sudah bersih.");
}

// ── IR CAPTURE & DECODE ─────────────────────────────────────
// Helper: parse sebuah field dari string hasil decode
String parseField(const String& src, const String& key) {
    int idx = src.indexOf(key);
    if (idx == -1) return "";
    int start = idx + key.length();
    int end = src.indexOf(',', start);
    if (end == -1) end = src.length();
    return src.substring(start, end);
}

bool captureAndDecode(String& line1, String& line2) {
    if (!irrecv.decode(&irResult)) return false;
    
    if (irResult.rawlen <= 10) {
        irrecv.resume();
        return false;
    }
    
    Serial.printf("[DEBUG IR] Sinyal tertangkap! Type: %d, Bits: %d, RawLen: %d\n", 
                  irResult.decode_type, irResult.bits, irResult.rawlen);
                  
    // Decode sinyal AC menjadi teks yang mudah dibaca
    String fullDesc = IRAcUtils::resultAcToString(&irResult);
    if (fullDesc.length() == 0) {
        fullDesc = resultToHumanReadableBasic(&irResult);
    }
    Serial.println("====== HASIL DECODE ======");
    Serial.println(fullDesc);
    Serial.println("==========================");

    if (hasACState(irResult.decode_type)) {
        // Parse field-field dari hasil decode
        String powerRaw = parseField(fullDesc, "Power: ");
        String tempRaw  = parseField(fullDesc, "Temp: ");
        String modeRaw  = parseField(fullDesc, "Mode: ");
        String fanRaw   = parseField(fullDesc, "Fan: ");
        
        bool curPower = (powerRaw == "On");
        
        // Bersihkan Mode (misal: "3 (Cool)" -> "Cool")
        int mIdx1 = modeRaw.indexOf('(');
        int mIdx2 = modeRaw.indexOf(')');
        if (mIdx1 != -1 && mIdx2 != -1) modeRaw = modeRaw.substring(mIdx1 + 1, mIdx2);
        
        // Bersihkan Fan (misal: "15 (Auto)" -> "Auto")
        int fIdx1 = fanRaw.indexOf('(');
        int fIdx2 = fanRaw.indexOf(')');
        if (fIdx1 != -1 && fIdx2 != -1) fanRaw = fanRaw.substring(fIdx1 + 1, fIdx2);
        
        // Bangun teks untuk OLED (Logger Mode)
        // Baris 1: P:ON  T:24C
        line1 = String("PWR: ") + (curPower ? "ON " : "OFF") + "  " + tempRaw;
        
        // Baris 2: Cool | F:Auto
        line2 = modeRaw + " | F:" + fanRaw;
        
    } else {
        line1 = typeToString(irResult.decode_type);
        line2 = "Val: " + String((unsigned long)irResult.value, HEX);
    }
    
    return true;
}

// ── SETUP ────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("[IR Catcher] Boot...");

    Wire.begin(OLED_SDA, OLED_SCL);
    Wire.setClock(400000);

    u8g2.setBusClock(400000);
    u8g2.begin();
    u8g2.setFont(u8g2_font_6x10_tf);
    Serial.println("[IR Catcher] OLED OK");

    irrecv.enableIRIn();
    Serial.println("[IR Catcher] IR OK. Siap mendengarkan.");

    appState = STATE_STANDBY;
}

// ── LOOP ─────────────────────────────────────────────────────
void loop() {
    unsigned long now = millis();

    if (now - lastAnim >= ANIM_MS) {
        lastAnim = now;
        if (++animFrame > 255) animFrame = 0;
    }

    if (appState == STATE_DETECTED) {
        drawDetectedScreen();
        if (now - detectedAt >= DETECTED_SHOW_MS) {
            appState = STATE_STANDBY;
            Serial.println("\n[Standby] Menunggu sinyal...");
        }
        return;
    }

    if (appState == STATE_STANDBY) {
        drawStandbyScreen();

        String line1, line2;
        if (captureAndDecode(line1, line2)) {
            detectedName  = line1;
            detectedLine2 = line2;
            detectedAt    = now;
            appState      = STATE_DETECTED;
            Serial.printf("[OLED] %s | %s\n", line1.c_str(), line2.c_str());
            
            flushIR();
        }
        return;
    }
}
