/**
 * ============================================================
 *  Industrial AC Automation System — SLAVE v2.0
 *
 *  Perubahan v2.0:
 *    - Hapus fast-path hardcode MAC → pairing wajib setelah factory reset
 *    - Rule-based: AC OFF otomatis setelah 5 menit tanpa orang
 *    - Mode Auto/Manual: saat manual, logika otomatis dinonaktifkan
 *    - Command baru: CMD_AC_ON/OFF, CMD_TEMP_UP/DOWN, CMD_SET_MODE
 *    - SlavePayload tambah acState agar master/web tahu status AC
 *
 *  Perubahan v1.2:
 *    - SlavePayload tambah enrollStatus untuk feedback ke master
 *    - Setpoint suhu dibulatkan ke integer (sesuai kemampuan AC)
 *    - Stabilitas: kirim laporan segera setelah event RFID/proximity
 *    - Watchdog: auto reconnect jika master tidak merespons ping
 *    - LED indikator mode enroll/delete berkedip non-blocking
 *
 *  Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)
 *
 *  Hardware:
 *    ESP32
 *    RC522 RFID  → MOSI:23, MISO:19, SCK:18, SDA:5, RST:22
 *    IR Transmitter → GPIO 4
 *    DHT22        → GPIO 15
 *    Proximity sensor (aktif LOW) → GPIO 13
 *    LED Hijau    → GPIO 25
 *    LED Merah    → GPIO 26
 *    Buzzer       → GPIO 27
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <IRsend.h>
#include <IRremoteESP8266.h>

// ============================================================
//  PIN
// ============================================================
#define PIN_RFID_CS    5
#define PIN_RFID_RST   22
#define PIN_IR_SEND    4
#define PIN_DHT        15
#define PIN_PROXIMITY  13
#define PIN_LED_GREEN  25
#define PIN_LED_RED    26
#define PIN_BUZZER     27
#define DHT_TYPE       DHT22

// ============================================================
//  CONFIG
// ============================================================
#define MAX_CARDS           10
#define IR_BUF_SIZE         100
#define ENROLL_TIMEOUT_MS   30000
#define DHT_READ_MS         5000
#define REPORT_MS           2000
#define PROX_DEBOUNCE_MS    2000
#define MASTER_TIMEOUT_MS   60000  // kirim ulang laporan jika master lama tidak ping
#define EMPTY_ROOM_OFF_MS   300000 // 5 menit tanpa orang → AC OFF

#define T_TARGET    24.0f
#define K_FACTOR    1.0f   // 1 derajat per orang (bulat)
#define N_REF       1
#define T_MIN       18.0f
#define T_MAX       26.0f

// ============================================================
//  ENROLL STATUS
// ============================================================
#define ENROLL_IDLE       0
#define ENROLL_SUCCESS    1
#define ENROLL_DUPLICATE  2
#define ENROLL_FULL       3

// ============================================================
//  STRUCTS
// ============================================================
typedef struct {
    uint8_t  cmd;
    uint8_t  slaveIdx;
    uint32_t irData[IR_BUF_SIZE];
    uint16_t irLen;
    uint8_t  irType;
    uint8_t  cardUid[4];
} MasterCommand;

typedef struct {
    float    temp;
    float    hum;
    int      personCount;
    float    setTemp;
    uint8_t  cardCount;
    uint8_t  enrollStatus;
    uint8_t  acState;       // 0=off, 1=on
} SlavePayload;

#define CMD_SEND_IR         0x01
#define CMD_ENROLL_START    0x02
#define CMD_DELETE_CARDS    0x03
#define CMD_PING            0x04
#define CMD_DELETE_ONE_CARD 0x05
#define CMD_AC_ON           0x06
#define CMD_AC_OFF          0x07
#define CMD_TEMP_UP_CMD     0x08
#define CMD_TEMP_DOWN_CMD   0x09
#define CMD_SET_MODE        0x0A

#define IR_TYPE_ON          0
#define IR_TYPE_OFF         1
#define IR_TYPE_TEMP_UP     2
#define IR_TYPE_TEMP_DOWN   3

// ============================================================
//  STATE
// ============================================================
enum SlaveState { SLAVE_INIT, SLAVE_PAIRING, SLAVE_IDLE, SLAVE_ENROLL, SLAVE_DELETE_ONE };
SlaveState slaveState = SLAVE_INIT;

// ============================================================
//  GLOBALS
// ============================================================
uint32_t irON[IR_BUF_SIZE];        uint16_t irON_len        = 0;
uint32_t irOFF[IR_BUF_SIZE];       uint16_t irOFF_len       = 0;
uint32_t irTEMP_UP[IR_BUF_SIZE];   uint16_t irTEMP_UP_len   = 0;
uint32_t irTEMP_DOWN[IR_BUF_SIZE]; uint16_t irTEMP_DOWN_len = 0;

bool irON_ready = false, irOFF_ready = false;
bool irTEMP_UP_ready = false, irTEMP_DOWN_ready = false;

uint8_t  registeredCards[MAX_CARDS][4];
uint8_t  cardCount    = 0;
int      personCount  = 0;
float    currentTemp  = 0;
float    currentHum   = 0;
float    setTemp      = T_TARGET;
uint8_t  enrollStatus = ENROLL_IDLE;

// AC state tracking
bool          acIsOn           = false;  // status AC saat ini
bool          roomEmpty        = false;  // ruangan kosong, timer berjalan
unsigned long emptyRoomStart   = 0;      // waktu mulai ruangan kosong
bool          autoMode         = true;   // true=auto, false=manual

unsigned long lastProxTrigger  = 0;
bool          lastProxState    = HIGH;
unsigned long lastDhtRead      = 0;
unsigned long lastReport       = 0;
unsigned long lastMasterSeen   = 0;  // timestamp terakhir terima perintah dari master
bool          reportNow        = false;
bool          irAllReady       = false;  // semua IR diterima, AC ON dari loop
bool          enrollFeedback   = false;  // feedback enroll dari loop
bool          deleteFeedback   = false;  // feedback delete dari loop

// Manual command flags (set dari callback, eksekusi di loop)
volatile uint8_t manualCmdPending = 0;  // 0=none, CMD_AC_ON/OFF/TEMP_UP/DOWN

// Enroll & delete one
bool          enrollMode       = false;
bool          deleteOneMode    = false;
unsigned long enrollStart      = 0;
unsigned long deleteOneStart   = 0;

// LED non-blocking
unsigned long lastLedBlink     = 0;
bool          ledBlinkState    = false;

uint8_t  masterMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
bool     masterKnown  = false;
uint8_t  masterCh     = 1;  // channel master, update saat pairing

Preferences prefs;
MFRC522     rfid(PIN_RFID_CS, PIN_RFID_RST);
DHT         dht(PIN_DHT, DHT_TYPE);
IRsend      irsend(PIN_IR_SEND);

// ============================================================
//  FEEDBACK
// ============================================================
void ledBlink(int pin, int times, int onMs = 150, int offMs = 150) {
    for (int i = 0; i < times; i++) {
        digitalWrite(pin, HIGH); delay(onMs);
        digitalWrite(pin, LOW);
        if (i < times - 1) delay(offMs);
    }
}

void buzzerBeep(int times, int onMs = 100, int offMs = 100) {
    for (int i = 0; i < times; i++) {
        digitalWrite(PIN_BUZZER, HIGH); delay(onMs);
        digitalWrite(PIN_BUZZER, LOW);
        if (i < times - 1) delay(offMs);
    }
}

void feedbackOK()           { ledBlink(PIN_LED_GREEN, 1, 300); buzzerBeep(1, 100); }
void feedbackDuplicate()    { ledBlink(PIN_LED_GREEN, 2, 150); buzzerBeep(2, 80); }
void feedbackError()        { ledBlink(PIN_LED_RED, 3, 200);   buzzerBeep(3, 200, 200); }
void feedbackEnter()        { ledBlink(PIN_LED_GREEN, 1, 100); buzzerBeep(1, 50); }
void feedbackExit()         { ledBlink(PIN_LED_RED, 1, 100); }
void feedbackEnrollMode()   { ledBlink(PIN_LED_GREEN, 3, 80, 80); buzzerBeep(2, 80, 80); }
void feedbackDeleteMode()   { ledBlink(PIN_LED_RED, 3, 80, 80);   buzzerBeep(3, 80, 80); }
void feedbackCardDeleted()  { ledBlink(PIN_LED_RED, 2, 200);       buzzerBeep(2, 150, 100); }
void feedbackCardNotFound() { ledBlink(PIN_LED_RED, 5, 80, 80);    buzzerBeep(1, 500); }

// ============================================================
//  NVS
// ============================================================
void saveCards() {
    prefs.begin("cards", false);
    prefs.putUChar("count", cardCount);
    for (int i = 0; i < cardCount; i++) {
        char key[8]; snprintf(key, sizeof(key), "c%d", i);
        prefs.putBytes(key, registeredCards[i], 4);
    }
    prefs.end();
}

void loadCards() {
    prefs.begin("cards", true);
    cardCount = prefs.getUChar("count", 0);
    for (int i = 0; i < cardCount; i++) {
        char key[8]; snprintf(key, sizeof(key), "c%d", i);
        prefs.getBytes(key, registeredCards[i], 4);
    }
    prefs.end();
    Serial.printf("[NVS] Loaded %d kartu\n", cardCount);
}

void deleteAllCards() {
    prefs.begin("cards", false); prefs.clear(); prefs.end();
    cardCount = 0;
    memset(registeredCards, 0, sizeof(registeredCards));
    Serial.println("[RFID] Semua kartu dihapus");
}

bool deleteOneCard(uint8_t* uid) {
    for (int i = 0; i < cardCount; i++) {
        if (memcmp(registeredCards[i], uid, 4) == 0) {
            for (int j = i; j < cardCount - 1; j++)
                memcpy(registeredCards[j], registeredCards[j+1], 4);
            cardCount--;
            saveCards();
            Serial.printf("[RFID] Kartu dihapus, sisa: %d\n", cardCount);
            return true;
        }
    }
    return false;
}

void saveIRData() {
    prefs.begin("irdata", false);
    prefs.putBytes("irON",  irON,  irON_len  * sizeof(uint32_t)); prefs.putUShort("irON_len",  irON_len);
    prefs.putBytes("irOFF", irOFF, irOFF_len * sizeof(uint32_t)); prefs.putUShort("irOFF_len", irOFF_len);
    prefs.putBytes("irUP",  irTEMP_UP,   irTEMP_UP_len   * sizeof(uint32_t)); prefs.putUShort("irUP_len",  irTEMP_UP_len);
    prefs.putBytes("irDN",  irTEMP_DOWN, irTEMP_DOWN_len * sizeof(uint32_t)); prefs.putUShort("irDN_len",  irTEMP_DOWN_len);
    prefs.end();
    Serial.println("[NVS] IR data tersimpan");
}

void loadIRData() {
    prefs.begin("irdata", true);
    irON_len        = prefs.getUShort("irON_len", 0);
    irOFF_len       = prefs.getUShort("irOFF_len", 0);
    irTEMP_UP_len   = prefs.getUShort("irUP_len", 0);
    irTEMP_DOWN_len = prefs.getUShort("irDN_len", 0);
    if (irON_len)        { prefs.getBytes("irON",  irON,  irON_len*4);        irON_ready=true; }
    if (irOFF_len)       { prefs.getBytes("irOFF", irOFF, irOFF_len*4);       irOFF_ready=true; }
    if (irTEMP_UP_len)   { prefs.getBytes("irUP",  irTEMP_UP,   irTEMP_UP_len*4);   irTEMP_UP_ready=true; }
    if (irTEMP_DOWN_len) { prefs.getBytes("irDN",  irTEMP_DOWN, irTEMP_DOWN_len*4); irTEMP_DOWN_ready=true; }
    prefs.end();
    Serial.printf("[NVS] IR: ON=%d OFF=%d UP=%d DN=%d\n",
        irON_len, irOFF_len, irTEMP_UP_len, irTEMP_DOWN_len);
}

// Track kartu yang sedang di dalam ruangan
uint8_t  insideCards[MAX_CARDS][4];
uint8_t  insideCount = 0;

bool cardInsideExists(uint8_t* uid) {
    for (int i = 0; i < insideCount; i++)
        if (memcmp(insideCards[i], uid, 4) == 0) return true;
    return false;
}

void addCardInside(uint8_t* uid) {
    if (insideCount >= MAX_CARDS) return;
    memcpy(insideCards[insideCount], uid, 4);
    insideCount++;
}

void removeFirstCardInside() {
    if (insideCount == 0) return;
    for (int i = 0; i < insideCount - 1; i++)
        memcpy(insideCards[i], insideCards[i+1], 4);
    insideCount--;
}

bool cardExists(uint8_t* uid) {
    for (int i = 0; i < cardCount; i++)
        if (memcmp(registeredCards[i], uid, 4) == 0) return true;
    return false;
}

void enrollCard(uint8_t* uid) {
    Serial.printf("[RFID] Enroll UID: %02X %02X %02X %02X\n",
        uid[0], uid[1], uid[2], uid[3]);
    if (cardExists(uid)) {
        Serial.println("[RFID] Duplikat");
        enrollStatus = ENROLL_DUPLICATE;
        feedbackDuplicate();
    } else if (cardCount >= MAX_CARDS) {
        Serial.println("[RFID] Penuh");
        enrollStatus = ENROLL_FULL;
        feedbackError();
    } else {
        memcpy(registeredCards[cardCount], uid, 4);
        cardCount++;
        saveCards();
        enrollStatus = ENROLL_SUCCESS;
        Serial.printf("[RFID] Berhasil, total: %d\n", cardCount);
        feedbackOK();
    }
    reportNow = true; // kirim status segera ke master
}

// ============================================================
//  AC CONTROL
// ============================================================
float calculateSetTemp(int n) {
    float t = T_TARGET - K_FACTOR * (n - N_REF);
    t = round(t); // bulatkan ke integer
    if (t < T_MIN) t = T_MIN;
    if (t > T_MAX) t = T_MAX;
    return t;
}

void sendIR(uint32_t* data, uint16_t len) {
    if (len == 0 || data == nullptr) return;
    uint16_t buf[IR_BUF_SIZE];
    for (int i = 0; i < len && i < IR_BUF_SIZE; i++)
        buf[i] = (uint16_t)(data[i]);
    irsend.sendRaw(buf, len, 38);
}

void sendACOn()    { if (irON_ready)        { sendIR(irON, irON_len);               Serial.println("[IR] AC ON"); } }
void sendACOff()   { if (irOFF_ready)       { sendIR(irOFF, irOFF_len);             Serial.println("[IR] AC OFF"); } }
void sendTempUp()  { if (irTEMP_UP_ready)   { sendIR(irTEMP_UP, irTEMP_UP_len);    Serial.println("[IR] TEMP UP"); } }
void sendTempDown(){ if (irTEMP_DOWN_ready) { sendIR(irTEMP_DOWN, irTEMP_DOWN_len);Serial.println("[IR] TEMP DOWN"); } }

void adjustACTemp(float currentSet, float targetSet) {
    int diff = (int)(targetSet - currentSet);
    if (diff == 0) return;
    Serial.printf("[AC] Adjust %.0f→%.0f (diff=%d)\n", currentSet, targetSet, diff);
    if (diff > 0) {
        for (int i = 0; i < diff; i++) {
            sendTempUp();
            // Yield ke watchdog tiap iterasi
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    } else {
        for (int i = 0; i < -diff; i++) {
            sendTempDown();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

// ============================================================
//  ESP-NOW
// ============================================================
void reportToMaster() {
    if (!masterKnown) return;
    if (masterCh == 0) masterCh = 1;
    esp_wifi_set_channel(masterCh, WIFI_SECOND_CHAN_NONE);

    SlavePayload payload;
    payload.temp         = currentTemp;
    payload.hum          = currentHum;
    payload.personCount  = personCount;
    payload.setTemp      = setTemp;
    payload.cardCount    = cardCount;
    payload.enrollStatus = enrollStatus;
    payload.acState      = acIsOn ? 1 : 0;

    esp_err_t result = esp_now_send(masterMac, (uint8_t*)&payload, sizeof(SlavePayload));
    Serial.printf("[REPORT] CH%d T:%.1f H:%.1f N:%d AC:%s result:%d\n",
        masterCh, payload.temp, payload.hum, payload.personCount,
        acIsOn ? "ON" : "OFF", result);
}

void onDataRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    lastMasterSeen = millis();

    uint8_t recvCh = 0;
    esp_wifi_get_channel(&recvCh, nullptr);
    if (recvCh == 0) recvCh = 1;

    // ── Pairing: terima ping master (9 byte: magic 0xAC57 + channel + MAC) ─
    if (len == 9 && !masterKnown && data[0] == 0xAC && data[1] == 0x57) {
        uint8_t masterChannel = data[2];          // channel master dari packet
        memcpy(masterMac, data + 3, 6);           // MAC master dari packet
        if (masterChannel == 0) masterChannel = 1;
        masterKnown = true;
        masterCh    = masterChannel;
        esp_wifi_set_channel(masterCh, WIFI_SECOND_CHAN_NONE);
        prefs.begin("sys", false);
        prefs.putBytes("masterMac", masterMac, 6);
        prefs.putBool("masterKnown", true);
        prefs.putUChar("channel", masterCh);
        prefs.end();
        if (esp_now_is_peer_exist(masterMac)) {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, masterMac, 6);
            peer.channel = masterCh; peer.encrypt = false;
            esp_now_mod_peer(&peer);
        } else {
            esp_now_peer_info_t peer = {};
            memcpy(peer.peer_addr, masterMac, 6);
            peer.channel = masterCh; peer.encrypt = false;
            esp_now_add_peer(&peer);
        }
        slaveState = SLAVE_IDLE;
        reportNow  = true;
        Serial.printf("[PAIRING] Master di CH%d MAC:%02X:%02X:%02X:%02X:%02X:%02X\n",
            masterCh,
            masterMac[0],masterMac[1],masterMac[2],
            masterMac[3],masterMac[4],masterMac[5]);
        return;
    }

    // Abaikan semua paket jika master belum dikenal
    if (!masterKnown) return;

    // Selalu update channel peer jika master kirim ping dengan channel baru
    // (tidak auto-update dari recvCh karena bisa tidak akurat)

    if (slaveState == SLAVE_PAIRING) {
        slaveState = SLAVE_IDLE;
        reportNow  = true;
        return;
    }

    // CMD
    if (len == sizeof(MasterCommand)) {
        MasterCommand cmd; memcpy(&cmd, data, sizeof(MasterCommand));
        switch (cmd.cmd) {
            case CMD_PING:
                reportNow = true;
                break;

            case CMD_SEND_IR: {
                switch (cmd.irType) {
                    case IR_TYPE_ON:
                        memcpy(irON, cmd.irData, cmd.irLen*4);
                        irON_len=cmd.irLen; irON_ready=true;
                        Serial.printf("[IR] Terima ON len=%d\n", irON_len); break;
                    case IR_TYPE_OFF:
                        memcpy(irOFF, cmd.irData, cmd.irLen*4);
                        irOFF_len=cmd.irLen; irOFF_ready=true;
                        Serial.printf("[IR] Terima OFF len=%d\n", irOFF_len); break;
                    case IR_TYPE_TEMP_UP:
                        memcpy(irTEMP_UP, cmd.irData, cmd.irLen*4);
                        irTEMP_UP_len=cmd.irLen; irTEMP_UP_ready=true;
                        Serial.printf("[IR] Terima TEMP_UP len=%d\n", irTEMP_UP_len); break;
                    case IR_TYPE_TEMP_DOWN:
                        memcpy(irTEMP_DOWN, cmd.irData, cmd.irLen*4);
                        irTEMP_DOWN_len=cmd.irLen; irTEMP_DOWN_ready=true;
                        Serial.printf("[IR] Terima TEMP_DOWN len=%d\n", irTEMP_DOWN_len); break;
                }
                if (irON_ready && irOFF_ready && irTEMP_UP_ready && irTEMP_DOWN_ready) {
                    saveIRData();
                    // feedback dan AC ON dilakukan dari loop via flag
                    irAllReady = true;
                }
                break;
            }

            case CMD_ENROLL_START:
                enrollMode    = true;
                deleteOneMode = false;
                enrollStatus  = ENROLL_IDLE;
                enrollStart   = millis();
                slaveState    = SLAVE_ENROLL;
                enrollFeedback = true; // feedback dari loop
                Serial.println("[ENROLL] Mode enroll aktif");
                break;

            case CMD_DELETE_CARDS:
                deleteAllCards();
                deleteFeedback = true;
                reportNow = true;
                break;

            case CMD_DELETE_ONE_CARD:
                deleteOneMode = true;
                enrollMode    = false;
                deleteOneStart = millis();
                slaveState    = SLAVE_DELETE_ONE;
                Serial.println("[DELETE_ONE] Mode hapus 1 kartu aktif");
                feedbackDeleteMode();
                break;

            case CMD_AC_ON:
                manualCmdPending = CMD_AC_ON;
                Serial.println("[CMD] AC ON dari master (manual)");
                break;

            case CMD_AC_OFF:
                manualCmdPending = CMD_AC_OFF;
                Serial.println("[CMD] AC OFF dari master (manual)");
                break;

            case CMD_TEMP_UP_CMD:
                manualCmdPending = CMD_TEMP_UP_CMD;
                Serial.println("[CMD] TEMP UP dari master (manual)");
                break;

            case CMD_TEMP_DOWN_CMD:
                manualCmdPending = CMD_TEMP_DOWN_CMD;
                Serial.println("[CMD] TEMP DOWN dari master (manual)");
                break;

            case CMD_SET_MODE:
                autoMode = (cmd.slaveIdx == 0);  // 0=auto, 1=manual
                Serial.printf("[CMD] Mode diubah ke: %s\n", autoMode ? "AUTO" : "MANUAL");
                if (autoMode) {
                    // Kembali ke auto, reset state
                    roomEmpty = false;
                }
                reportNow = true;
                break;
        }
    }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[SLAVE v2.0] Boot");
    btStop();

    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED,   OUTPUT);
    pinMode(PIN_BUZZER,    OUTPUT);
    pinMode(PIN_PROXIMITY, INPUT_PULLUP);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED,   LOW);
    digitalWrite(PIN_BUZZER,    LOW);

    ledBlink(PIN_LED_GREEN, 2, 200);
    buzzerBeep(1, 200);

    SPI.begin(18, 19, 23, PIN_RFID_CS);
    rfid.PCD_Init();
    byte ver = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.printf("[RFID] Version: 0x%02X %s\n", ver,
        (ver != 0x00 && ver != 0xFF) ? "(OK)" : "(GAGAL)");

    dht.begin();
    irsend.begin();

    // Re-init RFID setelah irsend.begin() untuk hindari konflik timer
    rfid.PCD_Init();
    Serial.println("[RFID] Re-init selesai");

    loadCards();
    loadIRData();

    prefs.begin("sys", true);
    masterKnown = prefs.getBool("masterKnown", false);
    if (masterKnown) {
        prefs.getBytes("masterMac", masterMac, 6);
        masterCh = prefs.getUChar("channel", 1);
        if (masterCh == 0) masterCh = 1;
    }
    prefs.end();

    // Jika NVS kosong (factory reset), slave wajib scan channel
    // dan menunggu broadcast pairing dari master.
    // Setelah pairing pertama berhasil, data disimpan ke NVS
    // sehingga reconnect berikutnya cepat.

    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.disconnect();
    delay(100);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESP-NOW] INIT GAGAL!");
        while (true) { ledBlink(PIN_LED_RED, 5, 100, 100); delay(1000); }
    }
    esp_now_register_recv_cb(onDataRecv);

    SlavePayload hello; memset(&hello, 0, sizeof(hello));
    hello.cardCount = cardCount;
    uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

    if (masterKnown) {
        uint8_t savedCh = 0;
        prefs.begin("sys", true); savedCh = prefs.getUChar("channel", 1); prefs.end();
        esp_wifi_set_channel(savedCh, WIFI_SECOND_CHAN_NONE);
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, masterMac, 6);
        peer.channel = savedCh; peer.encrypt = false;
        esp_now_add_peer(&peer);

        Serial.printf("[PAIRING] Master dikenal, coba CH%d\n", savedCh);
        slaveState = SLAVE_PAIRING;
        unsigned long verifyStart = millis();
        while (millis() - verifyStart < 10000 && slaveState != SLAVE_IDLE) {
            esp_now_send(masterMac, (uint8_t*)&hello, sizeof(SlavePayload));
            delay(500);
        }
        if (slaveState != SLAVE_IDLE) {
            Serial.println("[PAIRING] Master tidak merespons, scan ulang");
            masterKnown = false;
            if (esp_now_is_peer_exist(masterMac)) esp_now_del_peer(masterMac);
            goto scan_all_channels;
        }
    } else {
        scan_all_channels:
        slaveState = SLAVE_PAIRING;
        ledBlink(PIN_LED_GREEN, 3, 100, 100);
        Serial.println("[PAIRING] Scan semua channel...");
        bool paired = false;
        for (int round = 0; round < 5 && !paired; round++) {
            for (int ch = 1; ch <= 13 && !paired; ch++) {
                esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
                if (esp_now_is_peer_exist(broadcast)) esp_now_del_peer(broadcast);
                esp_now_peer_info_t bcastPeer = {};
                memcpy(bcastPeer.peer_addr, broadcast, 6);
                bcastPeer.channel = ch; bcastPeer.encrypt = false;
                esp_now_add_peer(&bcastPeer);

                Serial.printf("[SCAN] CH%d\n", ch);

                // Kirim SlavePayload sebagai perkenalan
                esp_now_send(broadcast, (uint8_t*)&hello, sizeof(SlavePayload));

                // Dengarkan respons selama 2 detik
                unsigned long listenStart = millis();
                while (millis() - listenStart < 2000 && !masterKnown) {
                    delay(10);
                }
                if (masterKnown) { paired = true; break; }
            }
        }
        if (!paired) {
            Serial.println("[PAIRING] Gagal!");
            ledBlink(PIN_LED_RED, 3, 300, 300);
        }
    }

    lastMasterSeen = millis();
    delay(2000);
    currentTemp = dht.readTemperature();
    currentHum  = dht.readHumidity();
    if (isnan(currentTemp)) currentTemp = 25.0;
    if (isnan(currentHum))  currentHum  = 50.0;
    Serial.printf("[DHT] Awal: %.1fC %.1f%%\n", currentTemp, currentHum);
    Serial.println("[SLAVE] Loop dimulai");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
    unsigned long now = millis();

    // ── Handle flag dari callback (tidak boleh pakai delay di callback) ──
    if (irAllReady) {
        irAllReady = false;
        feedbackOK();
        sendACOn();
        acIsOn = true;
        Serial.println("[IR] Semua IR siap, AC dinyalakan");
    }
    if (enrollFeedback) {
        enrollFeedback = false;
        enrollStart    = millis(); // reset di sini agar timeout dihitung dari setelah feedback
        feedbackEnrollMode();
    }
    if (deleteFeedback) {
        deleteFeedback = false;
        feedbackError();
    }

    // ── LED hijau nyala saat idle, mati saat tidak idle ─────
    if (slaveState == SLAVE_IDLE && !enrollMode && !deleteOneMode) {
        digitalWrite(PIN_LED_GREEN, HIGH);
    } else if (slaveState == SLAVE_PAIRING) {
        digitalWrite(PIN_LED_GREEN, LOW);
    }
    static unsigned long lastPairRetry = 0;
    static uint8_t currentScanCh = 1;
    if (slaveState == SLAVE_PAIRING && now - lastPairRetry > 3000) {
        lastPairRetry = now;
        SlavePayload hello; memset(&hello, 0, sizeof(hello));
        hello.cardCount = cardCount;
        uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        esp_wifi_set_channel(currentScanCh, WIFI_SECOND_CHAN_NONE);
        if (esp_now_is_peer_exist(broadcast)) esp_now_del_peer(broadcast);
        esp_now_peer_info_t bcastPeer = {};
        memcpy(bcastPeer.peer_addr, broadcast, 6);
        bcastPeer.channel = currentScanCh; bcastPeer.encrypt = false;
        esp_now_add_peer(&bcastPeer);
        esp_now_send(broadcast, (uint8_t*)&hello, sizeof(SlavePayload));
        currentScanCh++;
        if (currentScanCh > 13) currentScanCh = 1;
    }
    if (slaveState == SLAVE_PAIRING && masterKnown) slaveState = SLAVE_IDLE;

    // ── Watchdog: kirim ulang laporan jika master lama tidak ping ──
    if (masterKnown && slaveState == SLAVE_IDLE
        && now - lastMasterSeen > MASTER_TIMEOUT_MS) {
        Serial.println("[WATCHDOG] Master lama tidak merespons, kirim ulang laporan...");
        lastMasterSeen = now; // reset timer agar tidak spam
        reportNow = true;
    }

    // ── Handle manual command dari master ─────────────────────
    if (manualCmdPending != 0) {
        uint8_t cmd = manualCmdPending;
        manualCmdPending = 0;
        switch (cmd) {
            case CMD_AC_ON:
                sendACOn();
                acIsOn = true;
                roomEmpty = false;
                Serial.println("[MANUAL] AC ON");
                buzzerBeep(1, 100);
                break;
            case CMD_AC_OFF:
                sendACOff();
                acIsOn = false;
                Serial.println("[MANUAL] AC OFF");
                buzzerBeep(2, 100);
                break;
            case CMD_TEMP_UP_CMD:
                sendTempUp();
                setTemp = min(setTemp + 1.0f, T_MAX);
                Serial.printf("[MANUAL] TEMP UP → %.0f\n", setTemp);
                buzzerBeep(1, 50);
                break;
            case CMD_TEMP_DOWN_CMD:
                sendTempDown();
                setTemp = max(setTemp - 1.0f, T_MIN);
                Serial.printf("[MANUAL] TEMP DOWN → %.0f\n", setTemp);
                buzzerBeep(1, 50);
                break;
        }
        reportNow = true;
    }

    // ── Baca DHT berkala ──────────────────────────────────────
    if (now - lastDhtRead > DHT_READ_MS) {
        lastDhtRead = now;
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t)) { currentTemp = t; }
        else { Serial.println("[DHT] Baca gagal"); }
        if (!isnan(h)) currentHum = h;
        Serial.printf("[DHT] T:%.1fC H:%.1f%%\n", currentTemp, currentHum);
    }

    // ── Timer 5 menit: AC OFF jika ruangan kosong (hanya mode auto) ──
    if (autoMode && roomEmpty && acIsOn
        && (now - emptyRoomStart > EMPTY_ROOM_OFF_MS)) {
        Serial.println("[RULE] 5 menit tanpa orang, AC OFF!");
        sendACOff();
        acIsOn = false;
        setTemp = T_TARGET;
        roomEmpty = false;
        reportNow = true;
    }

    // ── Proximity sensor (hanya mode auto) ────────────────────
    bool proxNow = digitalRead(PIN_PROXIMITY);
    if (autoMode && proxNow == LOW && lastProxState == HIGH) {
        if (now - lastProxTrigger > PROX_DEBOUNCE_MS) {
            lastProxTrigger = now;
            if (personCount > 0) {
                personCount--;
                removeFirstCardInside(); // hapus kartu pertama yang masuk (FIFO)
                Serial.printf("[PROX] Orang keluar, total: %d\n", personCount);
                feedbackExit();
                if (personCount == 0) {
                    // Jangan langsung matikan AC, mulai timer 5 menit
                    roomEmpty = true;
                    emptyRoomStart = millis();
                    setTemp = T_TARGET;
                    Serial.println("[RULE] Ruangan kosong, timer 5 menit dimulai");
                } else {
                    float newTemp = calculateSetTemp(personCount);
                    adjustACTemp(setTemp, newTemp);
                    setTemp = newTemp;
                }
                reportNow = true; // kirim segera setelah event
            }
        }
    }
    lastProxState = proxNow;

    // ── RFID ──────────────────────────────────────────────────
    if (enrollMode) {
        static unsigned long lastRfidCheck = 0;
        if (now - lastRfidCheck > 500) {
            lastRfidCheck = now;
            Serial.println("[RFID] Cek kartu...");
        }
    }
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        uint8_t uid[4];
        memcpy(uid, rfid.uid.uidByte, 4);
        rfid.PICC_HaltA();
        rfid.PCD_StopCrypto1();

        Serial.printf("[RFID] UID: %02X %02X %02X %02X state=%d\n",
            uid[0], uid[1], uid[2], uid[3], (int)slaveState);

        if (enrollMode) {
            enrollCard(uid);
        } else if (deleteOneMode) {
            bool deleted = deleteOneCard(uid);
            if (deleted) { feedbackCardDeleted(); Serial.println("[DELETE_ONE] OK"); }
            else         { feedbackCardNotFound(); Serial.println("[DELETE_ONE] Tidak ditemukan"); }
            deleteOneMode = false;
            slaveState    = SLAVE_IDLE;
            reportNow     = true;
        } else if (autoMode) {
            // Mode auto: cek akses dan kontrol AC
            if (cardExists(uid)) {
                if (cardInsideExists(uid)) {
                    Serial.println("[RFID] Kartu sudah di dalam, diabaikan");
                    feedbackDuplicate(); // 2 kedip tanda kartu sudah di dalam
                } else {
                    personCount++;
                    addCardInside(uid);
                    Serial.printf("[RFID] Akses diterima, orang: %d\n", personCount);
                    feedbackEnter();

                    // Cancel timer empty room jika ada
                    roomEmpty = false;

                    float newTemp = calculateSetTemp(personCount);
                    if (!acIsOn) {
                        sendACOn();
                        acIsOn = true;
                        delay(3000);
                    }
                    adjustACTemp(setTemp, newTemp);
                    setTemp = newTemp;
                    reportNow = true;
                }
            } else {
                Serial.println("[RFID] Kartu tidak dikenal");
                feedbackError();
            }
        } else {
            // Mode manual: RFID hanya feedback, tidak kontrol AC
            if (cardExists(uid)) {
                feedbackEnter();
                Serial.println("[RFID] Manual mode: kartu dikenali tapi AC tidak dikontrol");
            } else {
                feedbackError();
                Serial.println("[RFID] Kartu tidak dikenal");
            }
        }
    }

    // ── Timeout enroll ────────────────────────────────────────
    if (enrollMode && millis() - enrollStart > ENROLL_TIMEOUT_MS) {
        enrollMode    = false;
        slaveState    = SLAVE_IDLE;
        enrollStatus  = ENROLL_IDLE;
        Serial.println("[ENROLL] Timeout");
        ledBlink(PIN_LED_GREEN, 1, 500);
        buzzerBeep(1, 300);
    }

    // ── Timeout delete one ────────────────────────────────────
    if (deleteOneMode && millis() - deleteOneStart > ENROLL_TIMEOUT_MS) {
        deleteOneMode = false;
        slaveState    = SLAVE_IDLE;
        Serial.println("[DELETE_ONE] Timeout");
        ledBlink(PIN_LED_RED, 1, 500);
    }

    // ── LED indikator mode (non-blocking) ─────────────────────
    if (enrollMode && now - lastLedBlink > 800) {
        lastLedBlink  = now;
        ledBlinkState = !ledBlinkState;
        digitalWrite(PIN_LED_GREEN, ledBlinkState ? HIGH : LOW);
        digitalWrite(PIN_LED_RED,   LOW);
    } else if (deleteOneMode && now - lastLedBlink > 800) {
        lastLedBlink  = now;
        ledBlinkState = !ledBlinkState;
        digitalWrite(PIN_LED_RED,   ledBlinkState ? HIGH : LOW);
        digitalWrite(PIN_LED_GREEN, LOW);
    } else if (!enrollMode && !deleteOneMode) {
        digitalWrite(PIN_LED_GREEN, LOW);
        digitalWrite(PIN_LED_RED,   LOW);
    }

    // ── Laporan ke master ─────────────────────────────────────
    if (reportNow || now - lastReport > REPORT_MS) {
        lastReport = now;
        reportNow  = false;
        reportToMaster();
        // Reset enrollStatus setelah dikirim
        if (enrollStatus != ENROLL_IDLE && !enrollMode) {
            enrollStatus = ENROLL_IDLE;
        }
    }
}
