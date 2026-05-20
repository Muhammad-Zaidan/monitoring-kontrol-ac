#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>

#define PIN_IR_TX   4
#define PIN_LED     5
#define PIN_BTN     14

IRsend irsend(PIN_IR_TX);

uint16_t irTest[] = {
    9000, 4500,
    560, 560, 560, 1680, 560, 560, 560, 1680,
    560, 1680, 560, 560, 560, 1680, 560, 1680,
    560
};

void setup() {
    Serial.begin(115200);
    delay(1000);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_BTN, INPUT_PULLUP);
    digitalWrite(PIN_LED, LOW);
    irsend.begin();

    Serial.println("\n=== DEBUG IR Remote 1 Button ===");

    // ── TEST 1: LED ──────────────────────────────────────────
    Serial.println("\n[TEST 1] LED GPIO5 kedip 5x...");
    for (int i = 0; i < 5; i++) {
        digitalWrite(PIN_LED, HIGH);
        Serial.printf("  LED ON  (%d/5)\n", i+1);
        delay(300);
        digitalWrite(PIN_LED, LOW);
        Serial.println("  LED OFF");
        delay(300);
    }
    Serial.println("[TEST 1] Selesai. Apakah LED kedip 5x?");

    // ── TEST 2: IR TX ────────────────────────────────────────
    Serial.println("\n[TEST 2] IR TX GPIO4 — kirim 5x burst");
    Serial.println("  Arahkan kamera depan HP ke LED IR sekarang...");
    delay(2000); // beri waktu siapkan kamera
    for (int i = 0; i < 5; i++) {
        digitalWrite(PIN_LED, HIGH);
        irsend.sendRaw(irTest, sizeof(irTest)/sizeof(irTest[0]), 38);
        digitalWrite(PIN_LED, LOW);
        Serial.printf("  IR burst %d/5 terkirim\n", i+1);
        delay(800);
    }
    Serial.println("[TEST 2] Selesai. Apakah kamera melihat cahaya ungu/putih?");

    // ── TEST 3: Button ───────────────────────────────────────
    Serial.println("\n[TEST 3] Button GPIO14");
    Serial.println("  Tekan button beberapa kali...");
    Serial.println("  Tidak ditekan=1, Ditekan=0");
}

void loop() {
    // RAW print tiap 300ms
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 300) {
        lastPrint = millis();
        int raw = digitalRead(PIN_BTN);
        Serial.printf("  BTN GPIO14 = %d", raw);
        if (raw == LOW) Serial.print("  ← DITEKAN");
        Serial.println();
    }

    // Deteksi tekan → kirim IR + feedback
    if (digitalRead(PIN_BTN) == LOW) {
        unsigned long pressStart = millis();
        while (digitalRead(PIN_BTN) == LOW) delay(10);
        unsigned long dur = millis() - pressStart;

        Serial.printf("\n>> Button dilepas, durasi: %lums\n", dur);

        if (dur < 500) {
            Serial.println(">> Kategori: PENDEK → akan kirim ON/OFF");
        } else if (dur < 1500) {
            Serial.println(">> Kategori: SEDANG → akan kirim TEMP UP");
        } else {
            Serial.println(">> Kategori: PANJANG → akan kirim TEMP DOWN");
        }

        // Kirim IR test
        Serial.println(">> Kirim IR burst...");
        digitalWrite(PIN_LED, HIGH);
        irsend.sendRaw(irTest, sizeof(irTest)/sizeof(irTest[0]), 38);
        digitalWrite(PIN_LED, LOW);
        Serial.println(">> IR selesai. Cek kamera apakah menyala saat LED GPIO5 ON?\n");
    }
}