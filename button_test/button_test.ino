/**
 * Test Button Master
 * BTN_UP  : GPIO 32
 * BTN_DOWN: GPIO 33
 * BTN_OK  : GPIO 34
 * Pull-up eksternal, aktif LOW
 */

#define BTN_UP    32
#define BTN_DOWN  33
#define BTN_OK    34

#define DEBOUNCE_MS 50

unsigned long btnLastTime[3]  = {0, 0, 0};
bool          btnLastState[3] = {HIGH, HIGH, HIGH};

bool btnPressed(int idx) {
    const int pins[3] = {BTN_UP, BTN_DOWN, BTN_OK};
    bool cur = digitalRead(pins[idx]);
    if (cur == LOW && btnLastState[idx] == HIGH) {
        if (millis() - btnLastTime[idx] > DEBOUNCE_MS) {
            btnLastTime[idx] = millis();
            btnLastState[idx] = LOW;
            return true;
        }
    }
    if (cur == HIGH) btnLastState[idx] = HIGH;
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(BTN_UP,   INPUT);
    pinMode(BTN_DOWN, INPUT);
    pinMode(BTN_OK,   INPUT);

    Serial.println("=== Button Test ===");
    Serial.println("Tekan button untuk test");
    Serial.println("Raw state awal:");
    Serial.printf("  UP:%d  DOWN:%d  OK:%d\n",
        digitalRead(BTN_UP),
        digitalRead(BTN_DOWN),
        digitalRead(BTN_OK));
}

void loop() {
    // Tampilkan raw state tiap 500ms
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint > 500) {
        lastPrint = millis();
        Serial.printf("RAW -> UP:%d DOWN:%d OK:%d\n",
            digitalRead(BTN_UP),
            digitalRead(BTN_DOWN),
            digitalRead(BTN_OK));
    }

    // Deteksi press
    if (btnPressed(0)) Serial.println(">> BTN UP ditekan!");
    if (btnPressed(1)) Serial.println(">> BTN DOWN ditekan!");
    if (btnPressed(2)) Serial.println(">> BTN OK ditekan!");
}
