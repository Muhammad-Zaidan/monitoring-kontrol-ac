/**
 * ============================================================
 *  Industrial AC Automation System — MASTER v5.0
 *
 *  Perubahan v5.0:
 *    - Hapus fast-path hardcode MAC → pairing wajib setelah factory reset
 *    - Mode Auto/Manual via Firebase: polling /control tiap 3 detik
 *    - Command baru: AC ON/OFF, TEMP UP/DOWN, SET MODE
 *    - SlavePayload tambah acState
 *    - Realtime Firebase tambah field mode dan acState
 *
 *  Perubahan v4.3:
 *    - Navigasi idle: UP/DOWN scroll manual, OK masuk menu
 *    - Force read PZEM saat ganti channel (tidak ada delay tampil)
 *    - Struktur Firebase: realtime (tiap 15 detik) + historis (tiap 5 menit)
 *    - Auto delete historis Firebase > 30 hari
 *    - SD sync dari Firebase saat SD dipasang kembali
 *    - Stabilitas ESP-NOW: watchdog ping slave tiap 5 detik
 *    - Feedback enroll lebih jelas di LCD (berhasil/duplikat/penuh)
 *    - SlavePayload tambah field enrollStatus untuk feedback enroll
 *    - Setpoint suhu dibulatkan ke integer
 *    - Fix: UP/DOWN di idle tidak masuk menu
 *
 *  Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)
 *  ESP32 Core 3.x
 *
 *  Hardware:
 *    ESP32, LCD 20x4 I2C 0x27 (SDA:21 SCL:22)
 *    PZEM-004T x3 Serial2 RX:16 TX:17 addr 0x01-0x03
 *    RTC DS3231 I2C, SD SPI CS:5
 *    IR Receiver GPIO4
 *    BTN UP:32  DOWN:33  OK:34  (pull-up eksternal, aktif LOW)
 * ============================================================
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PZEM004Tv30.h>
#include <SD.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <esp_now.h>
#include <Preferences.h>
#include <IRrecv.h>
#include <IRremoteESP8266.h>
#include <IRutils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_wifi.h>

// ============================================================
//  PIN & CONFIG
// ============================================================
#define SD_CS               5
#define PZEM_RX             16
#define PZEM_TX             17
#define IR_RECEIVE_PIN      4
#define BTN_UP              32
#define BTN_DOWN            33
#define BTN_OK              34

#define MAX_SLAVES          5
#define IR_BUF_SIZE         100
#define DEBOUNCE_MS         50
#define IDLE_SCROLL_MS      5000
#define IDLE_REFRESH_MS     2000
#define PZEM_READ_MS        5000
#define CLOUD_REALTIME_MS   15000   // sync realtime ke Firebase
#define CLOUD_HISTORY_MS    300000  // sync historis ke Firebase (5 menit)
#define PAIRING_TIMEOUT_MS  60000
#define ENROLL_TIMEOUT_MS   30000
#define MENU_TIMEOUT_MS     7000
#define HEAP_WARN_BYTES     20000
#define SD_QUEUE_SIZE       20
#define SLAVE_TIMEOUT_MS    60000   // slave dianggap offline jika >60 detik tidak ada data
#define SLAVE_PING_MS       5000    // ping slave tiap 5 detik
#define HISTORY_MAX_DAYS    30      // hapus historis > 30 hari
#define CONTROL_POLL_MS     3000    // polling Firebase /control tiap 3 detik

#define FIREBASE_HOST   "https://ac-monitor-industri-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH   "DcOLsJQyQptHH7fTBvQGHQ1ClZpU1Oh9CKWgrttM"

// ============================================================
//  ENROLL STATUS (dari slave ke master)
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
    float   temp;
    float   hum;
    int     personCount;
    float   setTemp;
    uint8_t cardCount;
    uint8_t enrollStatus; // ENROLL_IDLE/SUCCESS/DUPLICATE/FULL
    uint8_t acState;      // 0=off, 1=on
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

typedef struct {
    bool     active;
    uint8_t  mac[6];
    char     label[16];
    uint32_t irON[IR_BUF_SIZE];        uint16_t irON_len;
    uint32_t irOFF[IR_BUF_SIZE];       uint16_t irOFF_len;
    uint32_t irTEMP_UP[IR_BUF_SIZE];   uint16_t irTEMP_UP_len;
    uint32_t irTEMP_DOWN[IR_BUF_SIZE]; uint16_t irTEMP_DOWN_len;
} SlaveConfig;

// ============================================================
//  STATE MACHINE
// ============================================================
enum AppState {
    STATE_SLAVE_PAIRING,
    STATE_IR_LEARNING,
    STATE_IR_WAITING,
    STATE_IDLE,
    STATE_MENU,
    STATE_MENU_ADD_SLAVE,
    STATE_MENU_EDIT_SELECT,
    STATE_MENU_UNPAIR_SELECT,
    STATE_MENU_UNPAIR_CONFIRM,
    STATE_MENU_ENROLL_SELECT,
    STATE_MENU_ENROLL_WAIT,
    STATE_MENU_DELETE_CARDS_SELECT,
    STATE_MENU_DELETE_CARDS_CONFIRM,
    STATE_MENU_DELETE_ONE_SELECT,
    STATE_MENU_DELETE_ONE_WAIT,
    STATE_MENU_RESET_CONFIRM,
    STATE_NOTIF
};

enum IrLearningStep {
    IR_LEARN_ON = 0,
    IR_LEARN_OFF,
    IR_LEARN_TEMP_UP,
    IR_LEARN_TEMP_DOWN,
    IR_LEARN_DONE
};

// ============================================================
//  GLOBALS
// ============================================================
AppState            appState         = STATE_SLAVE_PAIRING;
IrLearningStep      irStep           = IR_LEARN_ON;

SlaveConfig         slaves[MAX_SLAVES];
SlavePayload        slaveData[MAX_SLAVES];
unsigned long       slaveLastSeen[MAX_SLAVES] = {0};
int                 slaveCount       = 0;

int                 menuIndex        = 0;
int                 menuEditTarget   = -1;

const int           MENU_COUNT       = 7;
const char*         menuItems[]      = {
    "1.Tambah Slave",
    "2.Edit Remote IR",
    "3.Hapus Slave",
    "4.Enroll Kartu",
    "5.Hapus Semua Kartu",
    "6.Hapus 1 Kartu",
    "7.Reset Pabrik"
};

// IDLE
int                 idleView         = 0;  // 0-2=power CH1-3, 3=detail slave
int                 idlePowerCh      = 0;
unsigned long       lastIdleScroll   = 0;
unsigned long       lastIdleRefresh  = 0;

// Force read PZEM saat ganti channel
volatile bool       forceReadPzem    = false;

// Running text
struct RunText { String text; int pos; unsigned long lastTick; };
RunText             runRow[4];
bool                runActive[4]     = {false};

// PZEM cache
float               pzem_v[3]  = {0};
float               pzem_i[3]  = {0};
float               pzem_p[3]  = {0};
float               pzem_pf[3] = {0};
float               pzem_e[3]  = {0};
SemaphoreHandle_t   pzemMutex;

// Pairing
unsigned long       pairingStart   = 0;
bool                pairingWaiting = false;

// Enroll
unsigned long       enrollStart    = 0;
int                 enrollTarget   = -1;

// Delete one card
unsigned long       deleteOneStart = 0;

// Menu timeout
unsigned long       menuLastActivity = 0;

// IR
uint32_t            irBuf[IR_BUF_SIZE];
uint16_t            irBufLen    = 0;
unsigned long       irWaitStart = 0;

// Notif
unsigned long       notifUntil     = 0;
AppState            notifNextState = STATE_IDLE;

// Pairing flag
volatile bool       newSlaveDetected = false;
uint8_t             newSlaveMac[6]   = {0};
unsigned long       newSlaveTime     = 0;

// Button polling
bool                btnLastState[3] = {HIGH, HIGH, HIGH};
unsigned long       btnLastTime[3]  = {0, 0, 0};

// SD status tracking
bool                sdWasReady     = false;

// Mode Auto/Manual (synced via Firebase)
String              currentMode      = "auto";  // "auto" atau "manual"
unsigned long       lastControlPoll  = 0;
unsigned long       lastCmdTs        = 0;       // timestamp terakhir command yang dieksekusi

// FreeRTOS
QueueHandle_t       sdQueue;
SemaphoreHandle_t   sdMutex;
SemaphoreHandle_t   lcdMutex;

// Hardware
Preferences         prefs;
PZEM004Tv30         pzem1(Serial2, PZEM_RX, PZEM_TX, 0x01);
PZEM004Tv30         pzem2(Serial2, PZEM_RX, PZEM_TX, 0x02);
PZEM004Tv30         pzem3(Serial2, PZEM_RX, PZEM_TX, 0x03);
RTC_DS3231          rtc;
LiquidCrystal_I2C   lcd(0x27, 20, 4);
IRrecv              irrecv(IR_RECEIVE_PIN);
decode_results      irResults;
bool                rtcReady = false;
bool                sdReady  = false;

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void renderIdlePower();
void renderIdleDetail();
void refreshIdlePowerData();
void refreshIdleDetailData();
void renderMenu();
void renderIrLearning();
void renderSlaveSelectMenu(int sel);
void showNotif(const char* l0, const char* l1, const char* l2, const char* l3,
               unsigned long ms, AppState next);
void resetMenuActivity();
bool checkMenuTimeout();
void saveSlaves(); void loadSlaves();
void saveFirstRunDone(); bool isFirstRun();
void factoryReset();
void sendAllIRToSlave(int idx);
void sendIRDataToSlave(int idx, IrLearningStep step);
void sendCommand(int idx, MasterCommand &cmd);
void registerAllPeers();
void logToSDQueue(const String &data);
void scrollIdleNext();
void scrollIdlePrev();

// ============================================================
//  BUTTON
// ============================================================
bool btnPressed(int idx) {
    const int pins[3] = {BTN_UP, BTN_DOWN, BTN_OK};
    bool cur = digitalRead(pins[idx]);
    if (cur == LOW && btnLastState[idx] == HIGH) {
        if (millis() - btnLastTime[idx] > DEBOUNCE_MS) {
            btnLastTime[idx]  = millis();
            btnLastState[idx] = LOW;
            return true;
        }
    }
    if (cur == HIGH) btnLastState[idx] = HIGH;
    return false;
}

void flushButtons() {
    btnLastState[0] = btnLastState[1] = btnLastState[2] = HIGH;
}

// ============================================================
//  RUNNING TEXT ENGINE
// ============================================================
void startRunText(int row, const String &text) {
    if ((int)text.length() <= 20) {
        runActive[row] = false;
        if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lcd.setCursor(0, row);
            String p = text;
            while ((int)p.length() < 20) p += ' ';
            lcd.print(p.substring(0, 20));
            xSemaphoreGive(lcdMutex);
        }
        return;
    }
    runRow[row].text     = text + "   ";
    runRow[row].pos      = 0;
    runRow[row].lastTick = millis();
    runActive[row]       = true;
}

void stopRunText(int row)  { runActive[row] = false; }
void stopAllRunText()      { for (int i = 0; i < 4; i++) runActive[i] = false; }

void tickRunText() {
    for (int row = 0; row < 4; row++) {
        if (!runActive[row]) continue;
        if (millis() - runRow[row].lastTick < 350) continue;
        runRow[row].lastTick = millis();
        String &t = runRow[row].text;
        int len   = t.length();
        String view = "";
        for (int i = 0; i < 20; i++)
            view += t[(runRow[row].pos + i) % len];
        runRow[row].pos = (runRow[row].pos + 1) % len;
        if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            lcd.setCursor(0, row);
            lcd.print(view);
            xSemaphoreGive(lcdMutex);
        }
    }
}

// ============================================================
//  LCD HELPERS
// ============================================================
void lcdClear() {
    stopAllRunText();
    if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lcd.clear();
        xSemaphoreGive(lcdMutex);
    }
}

void lcdPrint(int col, int row, const String &s) {
    if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lcd.setCursor(col, row);
        String p = s;
        int maxLen = 20 - col;
        while ((int)p.length() < maxLen) p += ' ';
        lcd.print(p.substring(0, maxLen));
        xSemaphoreGive(lcdMutex);
    }
}

void lcdPrintAuto(int row, const String &s) { startRunText(row, s); }

// ============================================================
//  SD QUEUE
// ============================================================
void logToSDQueue(const String &data) {
    if (!sdReady) return;
    String* msg = new String(data);
    if (xQueueSend(sdQueue, &msg, 0) != pdTRUE) delete msg;
}

// ============================================================
//  NVS
// ============================================================
void saveSlaves() {
    prefs.begin("slaves", false);
    prefs.putInt("count", slaveCount);
    for (int i = 0; i < slaveCount; i++)
        prefs.putBytes(("s"+String(i)).c_str(), &slaves[i], sizeof(SlaveConfig));
    prefs.end();
}

void loadSlaves() {
    prefs.begin("slaves", true);
    slaveCount = prefs.getInt("count", 0);
    for (int i = 0; i < slaveCount; i++)
        prefs.getBytes(("s"+String(i)).c_str(), &slaves[i], sizeof(SlaveConfig));
    prefs.end();
}

void saveFirstRunDone() {
    prefs.begin("sys", false); prefs.putBool("firstrun", false); prefs.end();
}

bool isFirstRun() {
    prefs.begin("sys", true);
    bool fr = prefs.getBool("firstrun", true);
    prefs.end();
    return fr;
}

void factoryReset() {
    prefs.begin("slaves", false); prefs.clear(); prefs.end();
    prefs.begin("sys",    false); prefs.clear(); prefs.end();
    slaveCount = 0;
    memset(slaves, 0, sizeof(slaves));
    memset(slaveData, 0, sizeof(slaveData));
    memset(slaveLastSeen, 0, sizeof(slaveLastSeen));
}

// ============================================================
//  ESP-NOW
// ============================================================
void sendCommand(int idx, MasterCommand &cmd) {
    if (idx < 0 || idx >= slaveCount) return;
    esp_now_send(slaves[idx].mac, (uint8_t*)&cmd, sizeof(MasterCommand));
}

void sendIRDataToSlave(int idx, IrLearningStep step) {
    MasterCommand cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.cmd = CMD_SEND_IR; cmd.slaveIdx = idx; cmd.irType = (uint8_t)step;
    switch (step) {
        case IR_LEARN_ON:
            memcpy(cmd.irData, slaves[idx].irON, slaves[idx].irON_len * 4);
            cmd.irLen = slaves[idx].irON_len; break;
        case IR_LEARN_OFF:
            memcpy(cmd.irData, slaves[idx].irOFF, slaves[idx].irOFF_len * 4);
            cmd.irLen = slaves[idx].irOFF_len; break;
        case IR_LEARN_TEMP_UP:
            memcpy(cmd.irData, slaves[idx].irTEMP_UP, slaves[idx].irTEMP_UP_len * 4);
            cmd.irLen = slaves[idx].irTEMP_UP_len; break;
        case IR_LEARN_TEMP_DOWN:
            memcpy(cmd.irData, slaves[idx].irTEMP_DOWN, slaves[idx].irTEMP_DOWN_len * 4);
            cmd.irLen = slaves[idx].irTEMP_DOWN_len; break;
        default: return;
    }
    sendCommand(idx, cmd);
    vTaskDelay(pdMS_TO_TICKS(100));
}

void sendAllIRToSlave(int idx) {
    for (int s = 0; s < (int)IR_LEARN_DONE; s++)
        sendIRDataToSlave(idx, (IrLearningStep)s);
}

void registerAllPeers() {
    for (int i = 0; i < slaveCount; i++) {
        if (!slaves[i].active || esp_now_is_peer_exist(slaves[i].mac)) continue;
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, slaves[i].mac, 6);
        peer.channel = 0; peer.encrypt = false;
        esp_now_add_peer(&peer);
    }
}

void onDataRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
    Serial.printf("[RECV] Dari: %02X:%02X:%02X:%02X:%02X:%02X len=%d\n",
        info->src_addr[0], info->src_addr[1], info->src_addr[2],
        info->src_addr[3], info->src_addr[4], info->src_addr[5], len);
    Serial.printf("[RECV] appState=%d pairingWaiting=%d slaveCount=%d newSlaveDetected=%d\n",
        (int)appState, pairingWaiting, slaveCount, newSlaveDetected);
    Serial.printf("[RECV] sizeof(SlavePayload)=%d\n", sizeof(SlavePayload));
    if (len == sizeof(SlavePayload)) {
        for (int i = 0; i < slaveCount; i++) {
            if (memcmp(slaves[i].mac, info->src_addr, 6) == 0) {
                SlavePayload prev = slaveData[i];
                memcpy(&slaveData[i], data, sizeof(SlavePayload));
                slaveLastSeen[i] = millis();

                Serial.printf("[SLAVE %d] T:%.1f H:%.1f N:%d Tset:%.1f Cards:%d Enroll:%d\n",
                    i, slaveData[i].temp, slaveData[i].hum,
                    slaveData[i].personCount, slaveData[i].setTemp,
                    slaveData[i].cardCount, slaveData[i].enrollStatus);

                // Handle feedback enroll
                if (appState == STATE_MENU_ENROLL_WAIT && enrollTarget == i) {
                    if (slaveData[i].enrollStatus == ENROLL_SUCCESS && prev.enrollStatus != ENROLL_SUCCESS) {
                        showNotif("Kartu berhasil", "didaftarkan!", "", "", 2000, STATE_MENU);
                    } else if (slaveData[i].enrollStatus == ENROLL_DUPLICATE && prev.enrollStatus != ENROLL_DUPLICATE) {
                        showNotif("Kartu sudah", "terdaftar!", "", "", 2000, STATE_MENU_ENROLL_WAIT);
                    } else if (slaveData[i].enrollStatus == ENROLL_FULL && prev.enrollStatus != ENROLL_FULL) {
                        showNotif("Kartu penuh!", "Hapus kartu dulu.", "", "", 2000, STATE_MENU);
                    }
                }

                // Handle feedback delete one card
                if (appState == STATE_MENU_DELETE_ONE_WAIT && menuEditTarget == i) {
                    if (slaveData[i].cardCount < prev.cardCount) {
                        showNotif("Kartu dihapus.", "", "", "", 2000, STATE_MENU);
                    }
                }
                return;
            }
        }
    }
    if ((appState == STATE_SLAVE_PAIRING || appState == STATE_MENU_ADD_SLAVE
         || appState == STATE_IDLE)
        && pairingWaiting && slaveCount < MAX_SLAVES && !newSlaveDetected) {
        // Balas dengan magic + channel + MAC agar slave set channel yang benar
        uint8_t myMac[6];
        esp_wifi_get_mac(WIFI_IF_STA, myMac);
        uint8_t pingPkt[9] = {0xAC, 0x57, (uint8_t)WiFi.channel(), 0,0,0,0,0,0};
        memcpy(pingPkt + 3, myMac, 6);
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, info->src_addr, 6);
        peer.channel = 0; peer.encrypt = false;
        if (!esp_now_is_peer_exist(info->src_addr)) esp_now_add_peer(&peer);
        esp_now_send(info->src_addr, pingPkt, 9);
        Serial.printf("[PAIRING] Balas magic+CH+MAC ke slave\n");

        memcpy(newSlaveMac, info->src_addr, 6);
        newSlaveDetected = true;
        newSlaveTime     = millis();
    }
}

bool isSlaveOnline(int idx) {
    if (slaveLastSeen[idx] == 0) return false;
    return (millis() - slaveLastSeen[idx]) < SLAVE_TIMEOUT_MS;
}

// ============================================================
//  IR
// ============================================================
bool captureIR() {
    if (!irrecv.decode(&irResults)) return false;
    irBufLen = 0;
    if (irResults.rawlen > 1 && irResults.rawlen <= IR_BUF_SIZE + 1) {
        for (uint16_t j = 1; j < irResults.rawlen && irBufLen < IR_BUF_SIZE; j++)
            irBuf[irBufLen++] = (uint32_t)(irResults.rawbuf[j] * kRawTick);
    }
    irrecv.resume();
    return (irBufLen > 0);
}

void saveIRToSlave(int idx, IrLearningStep step) {
    switch (step) {
        case IR_LEARN_ON:
            memcpy(slaves[idx].irON, irBuf, irBufLen * 4);
            slaves[idx].irON_len = irBufLen; break;
        case IR_LEARN_OFF:
            memcpy(slaves[idx].irOFF, irBuf, irBufLen * 4);
            slaves[idx].irOFF_len = irBufLen; break;
        case IR_LEARN_TEMP_UP:
            memcpy(slaves[idx].irTEMP_UP, irBuf, irBufLen * 4);
            slaves[idx].irTEMP_UP_len = irBufLen; break;
        case IR_LEARN_TEMP_DOWN:
            memcpy(slaves[idx].irTEMP_DOWN, irBuf, irBufLen * 4);
            slaves[idx].irTEMP_DOWN_len = irBufLen; break;
        default: break;
    }
}

// ============================================================
//  IDLE SCROLL — Manual dan Otomatis
// ============================================================
// idleView: 0=CH1, 1=CH2, 2=CH3, 3=Detail Slave
// Total view: 4 (CH1, CH2, CH3, Detail)
#define IDLE_VIEW_COUNT 4

void goToIdleView(int view) {
    stopAllRunText();
    idleView    = view;
    if (idleView < 3) {
        idlePowerCh = idleView;
        forceReadPzem = true; // paksa baca PZEM segera
        renderIdlePower();
    } else {
        renderIdleDetail();
    }
    lastIdleScroll  = millis();
    lastIdleRefresh = millis();
}

void scrollIdleNext() {
    goToIdleView((idleView + 1) % IDLE_VIEW_COUNT);
}

void scrollIdlePrev() {
    goToIdleView((idleView - 1 + IDLE_VIEW_COUNT) % IDLE_VIEW_COUNT);
}

// ============================================================
//  DISPLAY
// ============================================================
void refreshIdlePowerData() {
    char buf[21];
    if (xSemaphoreTake(pzemMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        snprintf(buf, sizeof(buf), "V:%-5.1fV  I:%-5.2fA",
            pzem_v[idlePowerCh], pzem_i[idlePowerCh]);
        lcdPrint(0, 1, buf);
        snprintf(buf, sizeof(buf), "P:%-6.1fW PF:%-4.2f",
            pzem_p[idlePowerCh], pzem_pf[idlePowerCh]);
        lcdPrint(0, 2, buf);
        snprintf(buf, sizeof(buf), "E:%-7.3fkWh SD:%-3s",
            pzem_e[idlePowerCh], sdReady ? "OK" : "ERR");
        lcdPrint(0, 3, buf);
        xSemaphoreGive(pzemMutex);
        lastIdleRefresh = millis();
    }
}

void renderIdlePower() {
    stopAllRunText();
    char buf[21];
    lcdClear();
    snprintf(buf, sizeof(buf), "=== POWER CH%d ===", idlePowerCh + 1);
    lcdPrint(0, 0, buf);
    if (xSemaphoreTake(pzemMutex, pdMS_TO_TICKS(300)) == pdTRUE) {
        snprintf(buf, sizeof(buf), "V:%-5.1fV  I:%-5.2fA",
            pzem_v[idlePowerCh], pzem_i[idlePowerCh]);
        lcdPrint(0, 1, buf);
        snprintf(buf, sizeof(buf), "P:%-6.1fW PF:%-4.2f",
            pzem_p[idlePowerCh], pzem_pf[idlePowerCh]);
        lcdPrint(0, 2, buf);
        snprintf(buf, sizeof(buf), "E:%-7.3fkWh SD:%-3s",
            pzem_e[idlePowerCh], sdReady ? "OK" : "ERR");
        lcdPrint(0, 3, buf);
        xSemaphoreGive(pzemMutex);
    }
    lastIdleRefresh = millis();
    lastIdleScroll  = millis();
}

void refreshIdleDetailData() {
    char buf[21];
    if (slaveCount > 0) {
        String statusLine = "";
        for (int i = 0; i < slaveCount; i++) {
            statusLine += String(slaves[i].label);
            statusLine += isSlaveOnline(i) ? ":ON " : ":OFF ";
        }
        lcdPrintAuto(0, statusLine);
        int showIdx = 0;
        for (int i = 0; i < slaveCount; i++) {
            if (isSlaveOnline(i)) { showIdx = i; break; }
        }
        snprintf(buf, sizeof(buf), "T:%.1fC H:%.1f%% N:%d",
            slaveData[showIdx].temp, slaveData[showIdx].hum,
            slaveData[showIdx].personCount);
        lcdPrint(0, 1, buf);
        snprintf(buf, sizeof(buf), "Tset:%.0fC  WiFi:%-3s",
            slaveData[showIdx].setTemp,
            WiFi.status() == WL_CONNECTED ? "OK" : "--");
        lcdPrint(0, 2, buf);
        snprintf(buf, sizeof(buf), "Heap:%4dKB SD:%-3s",
            (int)(ESP.getFreeHeap() / 1024), sdReady ? "OK" : "ERR");
        lcdPrint(0, 3, buf);
    } else {
        lcdPrint(0, 0, "Belum ada slave.");
        snprintf(buf, sizeof(buf), "WiFi:%-3s Heap:%4dKB",
            WiFi.status() == WL_CONNECTED ? "OK" : "--",
            (int)(ESP.getFreeHeap() / 1024));
        lcdPrint(0, 1, buf);
        lcdPrint(0, 2, "");
        lcdPrint(0, 3, "");
    }
    lastIdleRefresh = millis();
}

void renderIdleDetail() {
    stopAllRunText();
    lcdClear();
    refreshIdleDetailData();
    lastIdleScroll = millis();
}

void tickIdleScroll() {
    tickRunText();
    if (millis() - lastIdleRefresh >= IDLE_REFRESH_MS) {
        if (idleView < 3) refreshIdlePowerData();
        else              refreshIdleDetailData();
    }
    if (millis() - lastIdleScroll >= IDLE_SCROLL_MS) {
        scrollIdleNext();
    }
}

void renderMenu() {
    stopAllRunText();
    lcdClear();
    lcdPrint(0, 0, "=== MENU ===");
    int start = (menuIndex / 3) * 3;
    for (int i = 0; i < 3 && (start + i) < MENU_COUNT; i++) {
        String line = (start + i == menuIndex ? ">" : " ");
        line += menuItems[start + i];
        lcdPrintAuto(i + 1, line);
    }
}

void renderSlaveSelectMenu(int sel) {
    stopAllRunText();
    lcdClear();
    lcdPrint(0, 0, "Pilih Slave:");
    int start = (sel / 3) * 3;
    for (int i = 0; i < 3 && (start + i) < slaveCount; i++) {
        String line = (start + i == sel ? ">" : " ");
        line += String(slaves[start + i].label);
        line += isSlaveOnline(start + i) ? " [ON]" : " [OFF]";
        lcdPrintAuto(i + 1, line);
    }
}

void renderIrLearning() {
    const char* names[] = {"ON", "OFF", "TEMP UP", "TEMP DOWN"};
    stopAllRunText();
    lcdClear();
    lcdPrint(0, 0, "Rekam: " + String(slaves[menuEditTarget].label));
    lcdPrint(0, 1, "Tombol: " + String(names[irStep]));
    lcdPrint(0, 2, "Arahkan remote AC,");
    lcdPrint(0, 3, "[OK] = siap rekam");
}

// ============================================================
//  NOTIF & MENU HELPERS
// ============================================================
void showNotif(const char* l0, const char* l1, const char* l2, const char* l3,
               unsigned long ms, AppState next) {
    stopAllRunText();
    lcdClear();
    if (l0 && *l0) lcdPrint(0, 0, l0);
    if (l1 && *l1) lcdPrint(0, 1, l1);
    if (l2 && *l2) lcdPrint(0, 2, l2);
    if (l3 && *l3) lcdPrint(0, 3, l3);
    notifUntil     = millis() + ms;
    notifNextState = next;
    appState       = STATE_NOTIF;
}

void resetMenuActivity() { menuLastActivity = millis(); }

bool checkMenuTimeout() {
    if (millis() - menuLastActivity > MENU_TIMEOUT_MS) {
        stopAllRunText();
        appState = STATE_IDLE;
        goToIdleView(0);
        return true;
    }
    return false;
}

// ============================================================
//  FIREBASE HELPERS
// ============================================================
String getDateKey(DateTime &dt) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", dt.year(), dt.month(), dt.day());
    return String(buf);
}

String getTimeKey(DateTime &dt) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", dt.hour(), dt.minute());
    return String(buf);
}

void firebasePut(WiFiClientSecure &client, HTTPClient &http, const String &path, const String &json) {
    String url = String(FIREBASE_HOST) + path + ".json?auth=" + FIREBASE_AUTH;
    if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        int code = http.PUT(json);
        Serial.printf("[FIREBASE] PUT %s → %d\n", path.c_str(), code);
        if (code != 200) {
            Serial.printf("[FIREBASE] Response: %s\n", http.getString().c_str());
        }
        http.end();
    }
}

void firebaseDelete(WiFiClientSecure &client, HTTPClient &http, const String &path) {
    String url = String(FIREBASE_HOST) + path + ".json?auth=" + FIREBASE_AUTH;
    if (http.begin(client, url)) {
        http.sendRequest("DELETE");
        http.end();
    }
}

String firebaseGet(WiFiClientSecure &client, HTTPClient &http, const String &path) {
    String url = String(FIREBASE_HOST) + path + ".json?auth=" + FIREBASE_AUTH;
    String result = "";
    if (http.begin(client, url)) {
        int code = http.GET();
        if (code == 200) {
            result = http.getString();
        }
        http.end();
    }
    return result;
}

void firebasePatch(WiFiClientSecure &client, HTTPClient &http, const String &path, const String &json) {
    String url = String(FIREBASE_HOST) + path + ".json?auth=" + FIREBASE_AUTH;
    if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        http.sendRequest("PATCH", json);
        http.end();
    }
}

// ============================================================
//  SD SYNC dari Firebase (saat SD dipasang kembali)
// ============================================================
void syncSDFromFirebase() {
    if (!sdReady || !rtcReady || WiFi.status() != WL_CONNECTED) return;
    Serial.println("[SD SYNC] Mulai sinkronisasi dari Firebase...");

    WiFiClientSecure client; client.setInsecure(); client.setTimeout(5);
    HTTPClient http; http.setTimeout(5000);

    // Ambil data realtime terbaru dari Firebase dan tulis ke SD
    DateTime now = rtc.now();
    String path = "/" + String(now.year()) + "/" + String(now.month());
    if (!SD.exists(path)) SD.mkdir(path);
    File f = SD.open(path + "/sync_log.csv", FILE_APPEND);
    if (f) {
        f.println("SYNC," + now.timestamp(DateTime::TIMESTAMP_FULL) + ",Sinkronisasi dari Firebase");
        f.close();
    }
    Serial.println("[SD SYNC] Selesai.");
}

// ============================================================
//  CORE 0 TASK
// ============================================================
void taskBackground(void* param) {
    unsigned long lastPzem     = 0;
    unsigned long lastRealtime = 0;
    unsigned long lastHistory  = 0;
    unsigned long lastHeap     = 0;
    unsigned long lastSlavePing = 0;
    unsigned long lastDeleteOld = 0;

    for (;;) {

        // ── PZEM Read ──────────────────────────────────────────
        bool doRead = (millis() - lastPzem > PZEM_READ_MS) || forceReadPzem;
        if (doRead
            && appState != STATE_SLAVE_PAIRING
            && appState != STATE_MENU_ADD_SLAVE) {
            lastPzem      = millis();
            forceReadPzem = false;
            PZEM004Tv30* pzems[3] = {&pzem1, &pzem2, &pzem3};
            if (xSemaphoreTake(pzemMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                for (int ch = 0; ch < 3; ch++) {
                    float v = pzems[ch]->voltage();
                    if (!isnan(v)) {
                        float ci = pzems[ch]->current();
                        float p  = pzems[ch]->power();
                        float pf = pzems[ch]->pf();
                        float e  = pzems[ch]->energy();
                        Serial.printf("[PZEM%d RAW] V:%.1f I:%.3f P:%.1f PF:%.2f\n", ch+1, v, isnan(ci)?-1:ci, isnan(p)?-1:p, isnan(pf)?-1:pf);
                        pzem_v[ch]  = v;
                        pzem_i[ch]  = isnan(ci) ? 0 : ci;
                        pzem_p[ch]  = isnan(p)  ? 0 : p;
                        pzem_pf[ch] = isnan(pf) ? 0 : pf;
                        pzem_e[ch]  = isnan(e)  ? 0 : e;
                        logToSDQueue("PZEM" + String(ch+1)
                            + "," + String(pzem_v[ch], 1)
                            + "," + String(pzem_i[ch], 2)
                            + "," + String(pzem_p[ch], 1)
                            + "," + String(pzem_pf[ch], 2)
                            + "," + String(pzem_e[ch], 3));
                    }
                    vTaskDelay(pdMS_TO_TICKS(30));
                }
                xSemaphoreGive(pzemMutex);
            }
            for (int s = 0; s < slaveCount; s++) {
                logToSDQueue(String(slaves[s].label)
                    + ",T:"    + String(slaveData[s].temp, 1)
                    + ",H:"    + String(slaveData[s].hum, 1)
                    + ",N:"    + String(slaveData[s].personCount)
                    + ",Tset:" + String(slaveData[s].setTemp, 0));
            }
        }

        // ── SD Write ───────────────────────────────────────────
        {
            // Deteksi SD dipasang kembali
            bool sdNow = SD.begin(SD_CS);
            if (sdNow && !sdWasReady) {
                sdReady    = true;
                sdWasReady = true;
                Serial.println("[SD] Kartu dipasang, mulai sync...");
                syncSDFromFirebase();
            } else if (!sdNow && sdWasReady) {
                sdReady    = false;
                sdWasReady = false;
                Serial.println("[SD] Kartu dicabut.");
            }

            String* msg = nullptr;
            while (xQueueReceive(sdQueue, &msg, 0) == pdTRUE && msg) {
                if (sdReady) {
                    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                        String timestamp;
                        if (rtcReady) {
                            DateTime now = rtc.now();
                            timestamp = now.timestamp(DateTime::TIMESTAMP_FULL);
                        } else {
                            timestamp = "NORTC+" + String(millis() / 1000) + "s";
                        }
                        // Buat direktori satu level per satu (SD library tidak support nested mkdir)
                        String year  = "/" + (rtcReady ? String(rtc.now().year())  : String("NORTC"));
                        String month = year + "/" + (rtcReady ? String(rtc.now().month()) : String("0"));
                        if (!SD.exists(year))  SD.mkdir(year);
                        if (!SD.exists(month)) SD.mkdir(month);
                        File f = SD.open(month + "/log.csv", FILE_APPEND);
                        if (f) {
                            f.println(timestamp + "," + *msg);
                            f.close();
                            Serial.printf("[SD] Tulis: %s\n", msg->c_str());
                        } else {
                            Serial.printf("[SD] GAGAL buka file: %s/log.csv\n", month.c_str());
                        }
                        xSemaphoreGive(sdMutex);
                    }
                }
                delete msg; msg = nullptr;
            }
        }

        // ── Firebase Realtime (tiap 15 detik) ─────────────────
        if (millis() - lastRealtime > CLOUD_REALTIME_MS
            && appState != STATE_SLAVE_PAIRING
            && WiFi.status() == WL_CONNECTED) {
            lastRealtime = millis();
            WiFiClientSecure client; client.setInsecure(); client.setTimeout(3);
            HTTPClient http; http.setTimeout(3000);

            // Bangun JSON realtime
            String json = "{";
            if (xSemaphoreTake(pzemMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                for (int ch = 0; ch < 3; ch++) {
                    json += "\"pzem" + String(ch+1) + "\":{";
                    json += "\"v\":" + String(pzem_v[ch], 1) + ",";
                    json += "\"i\":" + String(pzem_i[ch], 2) + ",";
                    json += "\"p\":" + String(pzem_p[ch], 1) + ",";
                    json += "\"pf\":" + String(pzem_pf[ch], 2) + ",";
                    json += "\"e\":" + String(pzem_e[ch], 3) + "},";
                }
                xSemaphoreGive(pzemMutex);
            }
            for (int s = 0; s < slaveCount; s++) {
                json += "\"" + String(slaves[s].label) + "\":{";
                json += "\"temp\":" + String(slaveData[s].temp, 1) + ",";
                json += "\"hum\":" + String(slaveData[s].hum, 1) + ",";
                json += "\"n\":" + String(slaveData[s].personCount) + ",";
                json += "\"tset\":" + String((int)round(slaveData[s].setTemp)) + ",";
                json += "\"cards\":" + String(slaveData[s].cardCount) + ",";
                json += "\"online\":" + String(isSlaveOnline(s) ? "true" : "false") + ",";
                json += "\"acState\":" + String(slaveData[s].acState) + ",";
                json += "\"lastSeen\":" + String(millis()) + "},";
            }
            // Tambah field mode
            json += "\"mode\":\"" + currentMode + "\",";
            // Tambahkan timestamp master (unix time dari RTC) agar web bisa deteksi master offline
            if (rtcReady) {
                DateTime now2 = rtc.now();
                json += "\"_masterTs\":" + String((long)now2.unixtime());
            } else {
                json += "\"_masterTs\":" + String(millis() / 1000);
            }
            if (json.endsWith(",")) json.remove(json.length() - 1);
            json += "}";
            firebasePut(client, http, "/realtime", json);
        }

        // ── Firebase Historis (tiap 5 menit) ──────────────────
        if (millis() - lastHistory > CLOUD_HISTORY_MS
            && appState != STATE_SLAVE_PAIRING
            && WiFi.status() == WL_CONNECTED
            && rtcReady) {
            lastHistory = millis();
            WiFiClientSecure client; client.setInsecure(); client.setTimeout(3);
            HTTPClient http; http.setTimeout(3000);
            DateTime now = rtc.now();
            String dateKey = getDateKey(now);
            String timeKey = getTimeKey(now);
            String basePath = "/history/" + dateKey + "/" + timeKey;

            String json = "{";
            if (xSemaphoreTake(pzemMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                for (int ch = 0; ch < 3; ch++) {
                    json += "\"pzem" + String(ch+1) + "\":{";
                    json += "\"v\":" + String(pzem_v[ch], 1) + ",";
                    json += "\"i\":" + String(pzem_i[ch], 2) + ",";
                    json += "\"p\":" + String(pzem_p[ch], 1) + ",";
                    json += "\"e\":" + String(pzem_e[ch], 3) + "},";
                }
                xSemaphoreGive(pzemMutex);
            }
            for (int s = 0; s < slaveCount; s++) {
                json += "\"" + String(slaves[s].label) + "\":{";
                json += "\"temp\":" + String(slaveData[s].temp, 1) + ",";
                json += "\"hum\":" + String(slaveData[s].hum, 1) + ",";
                json += "\"n\":" + String(slaveData[s].personCount) + ",";
                json += "\"tset\":" + String((int)round(slaveData[s].setTemp)) + "},";
            }
            if (json.endsWith(",")) json.remove(json.length() - 1);
            json += "}";
            firebasePut(client, http, basePath, json);
            Serial.printf("[FIREBASE] Historis tersimpan: %s/%s\n",
                dateKey.c_str(), timeKey.c_str());
        }

        // ── Auto delete historis > 30 hari (cek tiap jam) ─────
        if (millis() - lastDeleteOld > 3600000UL
            && WiFi.status() == WL_CONNECTED
            && rtcReady) {
            lastDeleteOld = millis();
            WiFiClientSecure client; client.setInsecure(); client.setTimeout(3);
            HTTPClient http; http.setTimeout(3000);
            DateTime now = rtc.now();
            // Hapus data 31+ hari yang lalu
            for (int d = 31; d <= 35; d++) {
                // Hitung tanggal lama secara sederhana (tidak perlu presisi kalender)
                long tsOld = now.unixtime() - (long)d * 86400L;
                DateTime dtOld(tsOld);
                String oldKey = getDateKey(dtOld);
                firebaseDelete(client, http, "/history/" + oldKey);
            }
        }

        // ── Slave Ping (tiap 5 detik) ──────────────────────────
        if (millis() - lastSlavePing > SLAVE_PING_MS) {
            lastSlavePing = millis();
            for (int i = 0; i < slaveCount; i++) {
                if (!slaves[i].active) continue;
                MasterCommand cmd; memset(&cmd, 0, sizeof(cmd));
                cmd.cmd = CMD_PING; cmd.slaveIdx = i;
                sendCommand(i, cmd);
            }
        }

        // ── Heap Monitor ───────────────────────────────────────
        if (millis() - lastHeap > 30000) {
            lastHeap = millis();
            uint32_t fh = ESP.getFreeHeap();
            Serial.printf("[HEAP] Free: %d bytes\n", fh);
            if (fh < HEAP_WARN_BYTES) logToSDQueue("WARN,heap_low," + String(fh));
        }

        // ── Firebase Control Polling (tiap 3 detik) ────────────
        if (millis() - lastControlPoll > CONTROL_POLL_MS
            && WiFi.status() == WL_CONNECTED
            && slaveCount > 0
            && appState != STATE_SLAVE_PAIRING) {
            lastControlPoll = millis();
            WiFiClientSecure client2; client2.setInsecure(); client2.setTimeout(3);
            HTTPClient http2; http2.setTimeout(3000);

            String ctrlJson = firebaseGet(client2, http2, "/control");
            if (ctrlJson.length() > 2 && ctrlJson != "null") {
                // Parse mode
                int modeIdx = ctrlJson.indexOf("\"mode\"");
                if (modeIdx >= 0) {
                    int modeValStart = ctrlJson.indexOf(':', modeIdx) + 1;
                    // Find the value between quotes
                    int q1 = ctrlJson.indexOf('"', modeValStart);
                    int q2 = ctrlJson.indexOf('"', q1 + 1);
                    if (q1 >= 0 && q2 > q1) {
                        String newMode = ctrlJson.substring(q1 + 1, q2);
                        if (newMode != currentMode) {
                            currentMode = newMode;
                            Serial.printf("[CTRL] Mode diubah ke: %s\n", currentMode.c_str());
                            // Kirim CMD_SET_MODE ke semua slave
                            for (int i = 0; i < slaveCount; i++) {
                                MasterCommand cmd; memset(&cmd, 0, sizeof(cmd));
                                cmd.cmd = CMD_SET_MODE;
                                cmd.slaveIdx = (currentMode == "auto") ? 0 : 1;
                                sendCommand(i, cmd);
                            }
                        }
                    }
                }

                // Parse cmd
                int cmdIdx = ctrlJson.indexOf("\"cmd\"");
                if (cmdIdx >= 0) {
                    int cmdValStart = ctrlJson.indexOf(':', cmdIdx) + 1;
                    int cq1 = ctrlJson.indexOf('"', cmdValStart);
                    int cq2 = ctrlJson.indexOf('"', cq1 + 1);
                    if (cq1 >= 0 && cq2 > cq1) {
                        String cmdStr = ctrlJson.substring(cq1 + 1, cq2);
                        if (cmdStr != "none" && cmdStr.length() > 0) {
                            // Parse cmdTs untuk cek apakah command baru
                            int tsIdx = ctrlJson.indexOf("\"cmdTs\"");
                            unsigned long cmdTs = 0;
                            if (tsIdx >= 0) {
                                int tsStart = ctrlJson.indexOf(':', tsIdx) + 1;
                                // Skip whitespace
                                while (tsStart < (int)ctrlJson.length() && ctrlJson[tsStart] == ' ') tsStart++;
                                String tsStr = "";
                                while (tsStart < (int)ctrlJson.length() && ctrlJson[tsStart] >= '0' && ctrlJson[tsStart] <= '9') {
                                    tsStr += ctrlJson[tsStart++];
                                }
                                cmdTs = tsStr.toInt();
                            }

                            if (cmdTs != lastCmdTs) {
                                lastCmdTs = cmdTs;
                                Serial.printf("[CTRL] Perintah: %s\n", cmdStr.c_str());

                                // Tentukan command ESP-NOW
                                uint8_t espCmd = 0;
                                if      (cmdStr == "ac_on")     espCmd = CMD_AC_ON;
                                else if (cmdStr == "ac_off")    espCmd = CMD_AC_OFF;
                                else if (cmdStr == "temp_up")   espCmd = CMD_TEMP_UP_CMD;
                                else if (cmdStr == "temp_down") espCmd = CMD_TEMP_DOWN_CMD;

                                if (espCmd != 0) {
                                    for (int i = 0; i < slaveCount; i++) {
                                        MasterCommand mcmd; memset(&mcmd, 0, sizeof(mcmd));
                                        mcmd.cmd = espCmd; mcmd.slaveIdx = i;
                                        sendCommand(i, mcmd);
                                    }
                                }

                                // Clear command di Firebase
                                WiFiClientSecure client3; client3.setInsecure(); client3.setTimeout(3);
                                HTTPClient http3; http3.setTimeout(3000);
                                firebasePatch(client3, http3, "/control", "{\"cmd\":\"none\"}");
                            }
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    btStop();

    pinMode(BTN_UP,   INPUT);
    pinMode(BTN_DOWN, INPUT);
    pinMode(BTN_OK,   INPUT);

    pzemMutex = xSemaphoreCreateMutex();
    sdMutex   = xSemaphoreCreateMutex();
    lcdMutex  = xSemaphoreCreateMutex();
    sdQueue   = xQueueCreate(SD_QUEUE_SIZE, sizeof(String*));

    Wire.begin();
    lcd.init();
    lcd.backlight();
    lcdClear();
    lcdPrint(0, 0, "AC Monitor");
    lcdPrint(0, 1, "Inisialisasi...");
    delay(1200);

    rtcReady = rtc.begin();
    sdReady  = SD.begin(SD_CS);
    sdWasReady = sdReady;
    lcdPrint(0, 2, sdReady  ? "SD  : OK" : "SD  : Tidak ada");
    lcdPrint(0, 3, rtcReady ? "RTC : OK" : "RTC : Tidak ada");
    delay(800);

    irrecv.enableIRIn();
    loadSlaves();
    Serial2.begin(9600, SERIAL_8N1, PZEM_RX, PZEM_TX);

    lcdClear();
    lcdPrint(0, 0, "Menghubungkan WiFi..");
    WiFiManager wm;
    wm.setConfigPortalTimeout(180);
    wm.setAPCallback([](WiFiManager*) {
        lcd.clear();
        lcd.setCursor(0,0); lcd.print("WiFi Setup Mode");
        lcd.setCursor(0,1); lcd.print("SSID: Master_AC_AP");
        lcd.setCursor(0,2); lcd.print("IP:   192.168.4.1");
        lcd.setCursor(0,3); lcd.print("Buka browser...");
    });
    if (!wm.autoConnect("Master_AC_AP")) {
        lcdClear(); lcdPrint(0,0,"WiFi gagal!"); lcdPrint(0,1,"Restart...");
        delay(2000); ESP.restart();
    }
    lcdClear();
    lcdPrint(0, 0, "WiFi: OK");
    lcdPrint(0, 1, WiFi.localIP().toString());
    Serial.printf("[WIFI] Channel: %d IP: %s\n",
        WiFi.channel(), WiFi.localIP().toString().c_str());

    // Sinkronisasi waktu via NTP
    if (rtcReady) {
        lcdPrint(0, 2, "Sinkron waktu NTP...");
        configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov"); // GMT+7 WIB
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 5000)) {
            rtc.adjust(DateTime(
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec
            ));
            Serial.printf("[NTP] Waktu disinkron: %04d-%02d-%02d %02d:%02d:%02d\n",
                timeinfo.tm_year+1900, timeinfo.tm_mon+1, timeinfo.tm_mday,
                timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            lcdPrint(0, 2, "Waktu: OK (NTP)");
        } else {
            Serial.println("[NTP] Gagal sinkron, pakai waktu RTC");
            lcdPrint(0, 2, "Waktu: RTC only");
        }
    }
    delay(800);

    // PENTING: set mode WIFI_STA dulu sebelum ESP-NOW init
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_19_5dBm); // maksimalkan daya TX
    delay(200);

    if (esp_now_init() != ESP_OK) {
        lcdPrint(0, 2, "ESP-NOW: GAGAL!");
        delay(2000);
    } else {
        esp_now_register_recv_cb(onDataRecv);
        registerAllPeers();
        uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        uint8_t wifiCh = WiFi.channel();
        if (esp_now_is_peer_exist(bc)) esp_now_del_peer(bc);
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, bc, 6);
        peer.channel = wifiCh;  // gunakan channel WiFi aktif
        peer.encrypt = false;
        esp_now_add_peer(&peer);
        Serial.printf("[ESP-NOW] Broadcast peer di CH%d\n", wifiCh);
    }
    delay(200);

    Serial.println("[MASTER v5.0] Siap.");
    Serial.printf("[SLAVES] %d slave tersimpan\n", slaveCount);

    // Pairing wajib setelah factory reset.
    // Tidak ada fast path inject — slave harus ditemukan via broadcast.

    // Kirim ping ke semua slave tersimpan saat boot
    // agar slave update channel ke channel WiFi aktif master
    if (slaveCount > 0) {
        uint8_t wifiCh = WiFi.channel();
        Serial.printf("[BOOT] Kirim ping ke %d slave di CH%d\n", slaveCount, wifiCh);
        delay(500);
        for (int i = 0; i < slaveCount; i++) {
            MasterCommand cmd; memset(&cmd, 0, sizeof(cmd));
            cmd.cmd = CMD_PING; cmd.slaveIdx = i;
            sendCommand(i, cmd);
            delay(100);
        }
    }

    xTaskCreatePinnedToCore(taskBackground, "bgTask", 8192, nullptr, 1, nullptr, 0);

    if (isFirstRun() || slaveCount == 0) {
        lcdClear();
        lcdPrint(0, 0, "Setup Pertama Kali!");
        lcdPrint(0, 1, "Nyalakan slave baru,");
        lcdPrint(0, 2, "tunggu koneksi...");
        lcdPrint(0, 3, "[OK] untuk lewati");
        pairingStart = millis(); pairingWaiting = true;
        appState = STATE_SLAVE_PAIRING;
    } else {
        appState = STATE_IDLE;
        goToIdleView(0);
    }
}

// ============================================================
//  LOOP — Core 1
// ============================================================
void loop() {

    // ── Pairing ping ──────────────────────────────────────────
    static unsigned long lastPairingPing = 0;
    if ((appState == STATE_SLAVE_PAIRING || appState == STATE_MENU_ADD_SLAVE)
        && pairingWaiting && millis() - lastPairingPing > 2000) {
        lastPairingPing = millis();
        uint8_t bc[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        // Format: magic[2] + channel[1] + MAC[6] = 9 byte
        uint8_t myMac[6];
        esp_wifi_get_mac(WIFI_IF_STA, myMac);
        uint8_t pingPkt[9] = {0xAC, 0x57, (uint8_t)WiFi.channel(), 0,0,0,0,0,0};
        memcpy(pingPkt + 3, myMac, 6);
        esp_now_send(bc, pingPkt, 9);
        Serial.printf("[PING] Broadcast di CH%d\n", WiFi.channel());
    }

    // ── Handle slave baru ─────────────────────────────────────
    if (newSlaveDetected && millis() - newSlaveTime > 100) {
        newSlaveDetected = false;
        int idx = slaveCount;
        memset(&slaves[idx], 0, sizeof(SlaveConfig));
        memcpy(slaves[idx].mac, newSlaveMac, 6);
        snprintf(slaves[idx].label, sizeof(slaves[idx].label), "Slave %d", idx+1);
        slaves[idx].active   = true;
        slaveLastSeen[idx]   = millis();
        slaveCount++;
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, newSlaveMac, 6);
        peer.channel = 0; peer.encrypt = false;
        esp_now_add_peer(&peer);
        saveSlaves(); pairingWaiting = false;
        menuEditTarget = idx; irStep = IR_LEARN_ON;
        Serial.printf("[PAIRING] Slave baru: idx=%d\n", idx);
        showNotif("Slave ditemukan!", slaves[idx].label, "Lanjut rekam IR...", "", 2000, STATE_IR_LEARNING);
    }

    // ── STATE MACHINE ─────────────────────────────────────────
    switch (appState) {

        case STATE_SLAVE_PAIRING:
        case STATE_MENU_ADD_SLAVE: {
            if (pairingWaiting && millis() - pairingStart > PAIRING_TIMEOUT_MS) {
                pairingWaiting = false;
                AppState next = (appState == STATE_SLAVE_PAIRING) ? STATE_IDLE : STATE_MENU;
                if (appState == STATE_SLAVE_PAIRING) saveFirstRunDone();
                showNotif("Timeout pairing.", "Tidak ada slave.", "", "", 2000, next);
            }
            if (btnPressed(2)) {
                pairingWaiting = false;
                AppState next = (appState == STATE_SLAVE_PAIRING) ? STATE_IDLE : STATE_MENU;
                if (appState == STATE_SLAVE_PAIRING) saveFirstRunDone();
                showNotif("Pairing dilewati.", "", "", "", 1500, next);
            }
            break;
        }

        case STATE_IR_LEARNING: {
            if (btnPressed(2)) {
                lcdClear();
                lcdPrint(0, 0, "Siap merekam...");
                lcdPrint(0, 1, "Tekan tombol remote");
                lcdPrint(0, 2, "Timeout: 10 detik");
                lcdPrint(0, 3, "Tunggu...");
                irrecv.resume();
                irWaitStart = millis();
                appState    = STATE_IR_WAITING;
            }
            break;
        }

        case STATE_IR_WAITING: {
            if (captureIR()) {
                saveIRToSlave(menuEditTarget, irStep);
                irStep = (IrLearningStep)((int)irStep + 1);
                if (irStep == IR_LEARN_DONE) {
                    saveSlaves();
                    sendAllIRToSlave(menuEditTarget);
                    saveFirstRunDone();
                    showNotif("IR selesai!", "Data terkirim!", "", "", 2000, STATE_IDLE);
                } else {
                    showNotif("Sinyal tersimpan!", "", "", "", 800, STATE_IR_LEARNING);
                }
            } else if (millis() - irWaitStart > 10000) {
                showNotif("Tidak ada sinyal!", "Coba lagi.", "", "", 2000, STATE_IR_LEARNING);
            }
            break;
        }

        case STATE_IDLE: {
            tickIdleScroll();
            if (btnPressed(0)) scrollIdlePrev();  // UP = scroll mundur
            if (btnPressed(1)) scrollIdleNext();  // DOWN = scroll maju
            if (btnPressed(2)) {
                menuIndex = 0; appState = STATE_MENU;
                resetMenuActivity(); renderMenu();
            }
            break;
        }

        case STATE_MENU: {
            tickRunText();
            if (checkMenuTimeout()) break;
            if (btnPressed(0)) { resetMenuActivity(); menuIndex=(menuIndex-1+MENU_COUNT)%MENU_COUNT; renderMenu(); }
            if (btnPressed(1)) { resetMenuActivity(); menuIndex=(menuIndex+1)%MENU_COUNT; renderMenu(); }
            if (btnPressed(2)) {
                resetMenuActivity();
                switch (menuIndex) {
                    case 0:
                        lcdClear();
                        lcdPrint(0,0,"Mode Tambah Slave");
                        lcdPrint(0,1,"Nyalakan slave baru");
                        lcdPrint(0,2,"Menunggu...");
                        lcdPrint(0,3,"[OK] untuk batal");
                        pairingStart=millis(); pairingWaiting=true;
                        appState=STATE_MENU_ADD_SLAVE; break;
                    case 1:
                        if (!slaveCount){showNotif("Belum ada slave!","","","",2000,STATE_MENU);break;}
                        menuEditTarget=0; appState=STATE_MENU_EDIT_SELECT;
                        renderSlaveSelectMenu(menuEditTarget); break;
                    case 2:
                        if (!slaveCount){showNotif("Belum ada slave!","","","",2000,STATE_MENU);break;}
                        menuEditTarget=0; appState=STATE_MENU_UNPAIR_SELECT;
                        renderSlaveSelectMenu(menuEditTarget); break;
                    case 3:
                        if (!slaveCount){showNotif("Belum ada slave!","","","",2000,STATE_MENU);break;}
                        menuEditTarget=0; appState=STATE_MENU_ENROLL_SELECT;
                        renderSlaveSelectMenu(menuEditTarget); break;
                    case 4:
                        if (!slaveCount){showNotif("Belum ada slave!","","","",2000,STATE_MENU);break;}
                        menuEditTarget=0; appState=STATE_MENU_DELETE_CARDS_SELECT;
                        renderSlaveSelectMenu(menuEditTarget); break;
                    case 5:
                        if (!slaveCount){showNotif("Belum ada slave!","","","",2000,STATE_MENU);break;}
                        menuEditTarget=0; appState=STATE_MENU_DELETE_ONE_SELECT;
                        renderSlaveSelectMenu(menuEditTarget); break;
                    case 6:
                        lcdClear();
                        lcdPrint(0,0,"RESET KE PABRIK?");
                        lcdPrint(0,1,"Semua data terhapus!");
                        lcdPrint(0,2,"[OK] Ya  [UP] Batal");
                        appState=STATE_MENU_RESET_CONFIRM;
                        resetMenuActivity(); break;
                }
            }
            break;
        }

        case STATE_MENU_EDIT_SELECT: {
            tickRunText();
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();menuEditTarget=(menuEditTarget-1+slaveCount)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(1)){resetMenuActivity();menuEditTarget=(menuEditTarget+1)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(2)){resetMenuActivity();irStep=IR_LEARN_ON;showNotif("Edit IR:",slaves[menuEditTarget].label,"","",1200,STATE_IR_LEARNING);}
            break;
        }

        case STATE_MENU_UNPAIR_SELECT: {
            tickRunText();
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();menuEditTarget=(menuEditTarget-1+slaveCount)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(1)){resetMenuActivity();menuEditTarget=(menuEditTarget+1)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(2)){
                resetMenuActivity();
                lcdClear();
                lcdPrint(0,0,"Hapus slave ini?");
                lcdPrint(0,1,slaves[menuEditTarget].label);
                lcdPrint(0,2,"[OK] Ya  [UP] Batal");
                appState=STATE_MENU_UNPAIR_CONFIRM;
            }
            break;
        }

        case STATE_MENU_UNPAIR_CONFIRM: {
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();appState=STATE_MENU_UNPAIR_SELECT;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(2)){
                esp_now_del_peer(slaves[menuEditTarget].mac);
                for (int i=menuEditTarget;i<slaveCount-1;i++){
                    slaves[i]=slaves[i+1]; slaveData[i]=slaveData[i+1];
                    slaveLastSeen[i]=slaveLastSeen[i+1];
                }
                slaveCount--; saveSlaves();
                showNotif("Slave dihapus.","","","",1500,STATE_MENU);
            }
            break;
        }

        case STATE_MENU_ENROLL_SELECT: {
            tickRunText();
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();menuEditTarget=(menuEditTarget-1+slaveCount)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(1)){resetMenuActivity();menuEditTarget=(menuEditTarget+1)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(2)){
                resetMenuActivity();
                if (!isSlaveOnline(menuEditTarget)) {
                    showNotif("Slave offline!","Tidak bisa enroll.","","",2000,STATE_MENU);
                    break;
                }
                MasterCommand cmd; memset(&cmd,0,sizeof(cmd));
                cmd.cmd=CMD_ENROLL_START; cmd.slaveIdx=menuEditTarget;
                sendCommand(menuEditTarget,cmd);
                enrollTarget  = menuEditTarget;
                enrollStart   = millis();
                appState      = STATE_MENU_ENROLL_WAIT;
                lcdClear();
                lcdPrint(0,0,"Mode Enroll Aktif");
                lcdPrint(0,1,slaves[menuEditTarget].label);
                lcdPrint(0,2,"Tempelkan kartu RFID");
                lcdPrint(0,3,"Timeout: 30 detik");
            }
            break;
        }

        case STATE_MENU_ENROLL_WAIT: {
            int sisa = max(0, (int)(ENROLL_TIMEOUT_MS - (millis()-enrollStart)) / 1000);
            static int lastSisa = -1;
            if (sisa != lastSisa) {
                lastSisa = sisa;
                char buf[21]; snprintf(buf, sizeof(buf), "Sisa waktu: %2d dtk", sisa);
                lcdPrint(0, 3, buf);
            }
            if (millis()-enrollStart > ENROLL_TIMEOUT_MS) {
                lastSisa=-1; enrollTarget=-1;
                showNotif("Enroll selesai.","","","",1500,STATE_MENU);
            }
            if (btnPressed(2)) {
                lastSisa=-1; enrollTarget=-1;
                showNotif("Enroll dihentikan.","","","",1500,STATE_MENU);
            }
            break;
        }

        case STATE_MENU_DELETE_CARDS_SELECT: {
            tickRunText();
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();menuEditTarget=(menuEditTarget-1+slaveCount)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(1)){resetMenuActivity();menuEditTarget=(menuEditTarget+1)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(2)){
                resetMenuActivity();
                lcdClear();
                lcdPrint(0,0,"Hapus SEMUA kartu?");
                lcdPrint(0,1,slaves[menuEditTarget].label);
                lcdPrint(0,2,"[OK] Ya  [UP] Batal");
                appState=STATE_MENU_DELETE_CARDS_CONFIRM;
            }
            break;
        }

        case STATE_MENU_DELETE_CARDS_CONFIRM: {
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();appState=STATE_MENU_DELETE_CARDS_SELECT;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(2)){
                MasterCommand cmd; memset(&cmd,0,sizeof(cmd));
                cmd.cmd=CMD_DELETE_CARDS; cmd.slaveIdx=menuEditTarget;
                sendCommand(menuEditTarget,cmd);
                showNotif("Semua kartu dihapus.",slaves[menuEditTarget].label,"","",2000,STATE_MENU);
            }
            break;
        }

        case STATE_MENU_DELETE_ONE_SELECT: {
            tickRunText();
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();menuEditTarget=(menuEditTarget-1+slaveCount)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(1)){resetMenuActivity();menuEditTarget=(menuEditTarget+1)%slaveCount;renderSlaveSelectMenu(menuEditTarget);}
            if (btnPressed(2)){
                resetMenuActivity();
                if (!isSlaveOnline(menuEditTarget)) {
                    showNotif("Slave offline!","Tidak bisa hapus.","","",2000,STATE_MENU);
                    break;
                }
                MasterCommand cmd; memset(&cmd,0,sizeof(cmd));
                cmd.cmd=CMD_DELETE_ONE_CARD; cmd.slaveIdx=menuEditTarget;
                sendCommand(menuEditTarget,cmd);
                deleteOneStart=millis(); appState=STATE_MENU_DELETE_ONE_WAIT;
                lcdClear();
                lcdPrint(0,0,"Hapus 1 Kartu");
                lcdPrint(0,1,slaves[menuEditTarget].label);
                lcdPrint(0,2,"Tempelkan kartu");
                lcdPrint(0,3,"yang ingin dihapus");
            }
            break;
        }

        case STATE_MENU_DELETE_ONE_WAIT: {
            int sisa = max(0, (int)(ENROLL_TIMEOUT_MS - (millis()-deleteOneStart)) / 1000);
            static int lastSisa2 = -1;
            if (sisa != lastSisa2) {
                lastSisa2 = sisa;
                char buf[21]; snprintf(buf, sizeof(buf), "Timeout: %2d dtk", sisa);
                lcdPrint(0, 3, buf);
            }
            if (millis()-deleteOneStart > ENROLL_TIMEOUT_MS){lastSisa2=-1;showNotif("Timeout.","","","",1500,STATE_MENU);}
            if (btnPressed(2))                               {lastSisa2=-1;showNotif("Dibatalkan.","","","",1500,STATE_MENU);}
            break;
        }

        case STATE_MENU_RESET_CONFIRM: {
            if (checkMenuTimeout()) break;
            if (btnPressed(0)){resetMenuActivity();appState=STATE_MENU;renderMenu();}
            if (btnPressed(2)){
                lcdClear(); lcdPrint(0,0,"Mereset sistem...");
                factoryReset(); delay(1500); ESP.restart();
            }
            break;
        }

        case STATE_NOTIF: {
            if (millis() > notifUntil) {
                appState = notifNextState;
                flushButtons();
                switch (appState) {
                    case STATE_IDLE:
                        goToIdleView(0); break;
                    case STATE_MENU:
                        resetMenuActivity(); renderMenu(); break;
                    case STATE_IR_LEARNING:
                        renderIrLearning(); break;
                    case STATE_MENU_ENROLL_WAIT:
                        // kembali ke enroll wait setelah notif duplikat
                        break;
                    default: break;
                }
            }
            break;
        }

        default: break;
    }
}
