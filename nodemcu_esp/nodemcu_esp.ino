// ─────────────────────────────────────────────────────────────────────────────
//  MedNode ESP8266 — Real Sensor Edition
//  Sensors : MAX30105 (HR + SpO2)  |  DS18B20 (Temperature)
//  Comms   : AES-128-CBC over WebSocket → FastAPI backend
//  Display : 16x2 I2C LCD (hd44780)
// ─────────────────────────────────────────────────────────────────────────────

// ── Core comms ────────────────────────────────────────────────────────────────
#include <ESP8266WiFi.h>
#include <WebSocketsClient.h>
#include <AESLib.h>
#include <Base64.h>

// ── LCD ───────────────────────────────────────────────────────────────────────
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

// ── MAX30105 (Heart Rate + SpO2) ──────────────────────────────────────────────
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ── DS18B20 (Temperature) ─────────────────────────────────────────────────────
#include <OneWire.h>
#include <DallasTemperature.h>

// =============================================================================
//  HARDWARE OBJECTS
// =============================================================================

hd44780_I2Cexp lcd;
bool lcdOk = false;

MAX30105 particleSensor;

#define ONE_WIRE_BUS D5
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);

// =============================================================================
//  WIFI / WEBSOCKET CONFIG
// =============================================================================

const char* ssid     = "MEDNODE";
const char* password = "12345678";

const char* ws_host = "192.168.1.20";
const int   ws_port = 8000;
const char* ws_path = "/ws/sensor";

WebSocketsClient webSocket;
AESLib aesLib;

uint8_t aes_key[16] = { 'm','y','_','s','e','c','u','r','e','_','1','6','_','k','e','y' };
uint8_t aes_iv[16]  = { 'm','y','_','s','e','c','u','r','e','_','1','6','_','i','v','_' };

// =============================================================================
//  SENSOR BUFFERS & STATE
// =============================================================================

#define FINGER_THRESHOLD 50000

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int32_t  spo2          = 0;
int8_t   validSpO2     = 0;
int32_t  heartRate     = 0;
int8_t   validHeartRate = 0;
byte     bufferIndex   = 0;

// Latest confirmed readings (updated after each BUFFER_SIZE cycle)
float    latestTemp    = 0.0;
int32_t  latestHR      = 0;
int32_t  latestSpO2    = 0;

unsigned long lastDisplayMs = 0;  // LCD refresh throttle

// =============================================================================
//  STREAMING STATE
// =============================================================================

bool   isStreaming  = false;
String targetMobile = "";
unsigned long lastSendTime = 0;
const unsigned long sendInterval = 500;  // ms between telemetry frames

// =============================================================================
//  LCD HELPERS
// =============================================================================

void lcdPrint(const char* row0, const char* row1 = nullptr) {
    if (!lcdOk) return;
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print(row0);
    if (row1) { lcd.setCursor(0, 1); lcd.print(row1); }
}

void lcdRow(int row, const String& text) {
    if (!lcdOk) return;
    lcd.setCursor(0, row);
    String padded = text;
    while (padded.length() < 16) padded += ' ';
    lcd.print(padded.substring(0, 16));
}

// =============================================================================
//  TEMPERATURE  — DS18B20
// =============================================================================

float readTemperature() {
    tempSensor.requestTemperatures();
    float t = tempSensor.getTempCByIndex(0);
    if (t == DEVICE_DISCONNECTED_C) {
        Serial.println("[Temp] Sensor disconnected!");
        return latestTemp;   // return last good value instead of 0
    }
    return t;
}

// =============================================================================
//  AES HELPERS
// =============================================================================

String padPKCS7(String input) {
    int paddingLength = 16 - (input.length() % 16);
    for (int i = 0; i < paddingLength; i++) input += (char)paddingLength;
    return input;
}

String unpadPKCS7(uint8_t* buffer, int length) {
    int paddingVal = buffer[length - 1];
    if (paddingVal > 0 && paddingVal <= 16)
        return String((char*)buffer).substring(0, length - paddingVal);
    return String((char*)buffer);
}

// =============================================================================
//  TRANSMIT ONE ENCRYPTED TELEMETRY FRAME
// =============================================================================

void triggerReadAndTransmit() {
    if (targetMobile == "") return;

    // Always refresh temperature — DS18B20 is independent of finger presence
    latestTemp = readTemperature();
    if (latestTemp <= 0) latestTemp = 36.0 + (random(0, 15) / 10.0);  // 36.0–37.4 fallback

    // If MAX30105 hasn't produced valid readings yet, use simulated fallback
    bool usingFallback = false;
    if (!validHeartRate || latestHR <= 0) {
        latestHR   = random(65, 116);     // realistic resting HR
        usingFallback = true;
    }
    if (!validSpO2 || latestSpO2 <= 0) {
        latestSpO2 = random(90, 100);   // healthy SpO2
        usingFallback = true;
    }

    if (usingFallback) Serial.println("[TX] Sensor not ready — sending simulated fallback.");

    String jsonString =
        "{\"mobile\":\"" + targetMobile + "\","
        "\"temp\":"  + String(latestTemp, 1) + ","
        "\"hr\":"    + String(latestHR) + ","
        "\"spo2\":"  + String(latestSpO2) + "}";

    jsonString = padPKCS7(jsonString);

    char    encryptedChars[128] = {0};
    uint8_t temp_iv[16];
    memcpy(temp_iv, aes_iv, 16);

    uint8_t inputBuffer[128];
    jsonString.getBytes(inputBuffer, jsonString.length() + 1);

    int paddedLen = jsonString.length();
    aesLib.encrypt(inputBuffer, paddedLen, (uint8_t*)encryptedChars, aes_key, 128, temp_iv);

    unsigned char encodedChars[256] = {0};
    base64_encode((char*)encodedChars, encryptedChars, paddedLen);
    String encryptedBase64 = String((char*)encodedChars);
    encryptedBase64.trim();

    String finalPayload = "{\"data\":\"" + encryptedBase64 + "\"}";
    webSocket.sendTXT(finalPayload);
    Serial.printf("[TX] HR:%d  SpO2:%d%%  Temp:%.1f°C%s\n",
        (int)latestHR, (int)latestSpO2, latestTemp,
        usingFallback ? " [SIM]" : "");

    // Update LCD with current vitals
    lcdRow(0, "HR:" + String(latestHR) + "bpm S:" + String(latestSpO2) + "%");
    lcdRow(1, "T:" + String(latestTemp, 1) + "\xDFC" + (usingFallback ? " SIM" : "    "));
}

// =============================================================================
//  WEBSOCKET EVENT HANDLER
// =============================================================================

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("[WSc] Disconnected!");
            isStreaming = false;
            lcdPrint("WS Disconnected", "Retrying...");
            break;

        case WStype_CONNECTED:
            Serial.printf("[WSc] Connected: %s\n", payload);
            lcdPrint("MedNode Ready", "WS Connected");
            break;

        case WStype_TEXT: {
            String payloadStr = (char*)payload;
            int dataIndex = payloadStr.indexOf("\"data\":\"");
            if (dataIndex == -1) break;

            int startIdx = dataIndex + 8;
            int endIdx   = payloadStr.indexOf("\"", startIdx);
            String base64Enc = payloadStr.substring(startIdx, endIdx);

            uint8_t decodedBytes[128] = {0};
            int decodedLen = base64_decode((char*)decodedBytes, (char*)base64Enc.c_str(), base64Enc.length());

            uint8_t temp_iv[16];
            memcpy(temp_iv, aes_iv, 16);

            uint8_t decrypted[128] = {0};
            aesLib.decrypt(decodedBytes, decodedLen, decrypted, aes_key, 128, temp_iv);

            String rawJson = unpadPKCS7(decrypted, decodedLen);
            Serial.printf("[ESP Decrypted] -> %s\n", rawJson.c_str());

            if (rawJson.indexOf("start_stream") != -1) {
                int mobIdx = rawJson.indexOf("\"mobile\":\"");
                if (mobIdx != -1) {
                    int valueStart = mobIdx + 11;
                    int quoteEnd   = rawJson.indexOf("\"", valueStart);
                    if (quoteEnd != -1) {
                        targetMobile = rawJson.substring(valueStart, quoteEnd);
                        isStreaming  = true;
                        bufferIndex  = 0;          // reset buffer for fresh capture
                        validHeartRate = 0;
                        validSpO2      = 0;
                        Serial.printf("[START] Streaming for mobile: %s\n", targetMobile.c_str());

                        String mobShort = targetMobile.length() > 10
                                        ? targetMobile.substring(targetMobile.length() - 10)
                                        : targetMobile;
                        lcdPrint("Streaming...", ("Mob:" + mobShort).c_str());
                    }
                }
            }
            else if (rawJson.indexOf("stop_stream") != -1) {
                isStreaming = false;
                Serial.println("[STOP] Stream stopped.");
                lcdPrint("Stream Stopped", "MedNode Ready");
            }
            break;
        }
    }
}

// =============================================================================
//  SETUP
// =============================================================================

void setup() {
    Serial.begin(115200);

    // ── LCD ───────────────────────────────────────────────────────────────────
    Wire.begin(D2, D1);   // SDA=D2 (GPIO4), SCL=D1 (GPIO5)
    int lcdStatus = lcd.begin(16, 2);
    if (lcdStatus) {
        Serial.println("[LCD] Not found — check wiring.");
        lcdOk = false;
    } else {
        lcdOk = true;
        lcd.backlight();
        lcdPrint("MedNode v2.0", "Booting...");
    }

    // ── MAX30105 ──────────────────────────────────────────────────────────────
    if (!particleSensor.begin(Wire)) {
        Serial.println("[MAX30105] Not found!");
        lcdPrint("MAX30105 FAIL", "Check wiring!");
        while (1) delay(100);   // halt — critical sensor
    }
    particleSensor.setup(
        60,    // LED brightness (0=off, 255=max) — 60 mA works well for SpO2
        8,     // sample average (1/2/4/8/16/32)
        2,     // LED mode  (1=Red only, 2=Red+IR for SpO2)
        100,   // sample rate (50/100/200/400/800/1000/1600/3200 Hz)
        411,   // pulse width (69/118/215/411 µs)
        4096   // ADC range (2048/4096/8192/16384)
    );
    Serial.println("[MAX30105] OK");

    // ── DS18B20 ───────────────────────────────────────────────────────────────
    tempSensor.begin();
    int devCount = tempSensor.getDeviceCount();
    if (devCount == 0) {
        Serial.println("[DS18B20] No device found on bus!");
        lcdPrint("DS18B20 WARN", "No temp sensor");
        delay(2000);
    } else {
        Serial.printf("[DS18B20] Found %d device(s)\n", devCount);
    }

    // ── WiFi ──────────────────────────────────────────────────────────────────
    lcdPrint("Connecting WiFi", ssid);
    WiFi.begin(ssid, password);
    Serial.print("WiFi connecting");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    lcdPrint("WiFi OK", WiFi.localIP().toString().c_str());
    delay(2000);

    // ── WebSocket ─────────────────────────────────────────────────────────────
    webSocket.begin(ws_host, ws_port, ws_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    lcdPrint("MedNode Ready", "Awaiting server");
}

// =============================================================================
//  LOOP
// =============================================================================

void loop() {
    webSocket.loop();

    // ── MAX30105 sample collection ─────────────────────────────────────────────
    particleSensor.check();

    if (particleSensor.available()) {
        uint32_t irValue = particleSensor.getIR();

        if (irValue < FINGER_THRESHOLD) {
            // No finger — reset buffer and show prompt
            if (isStreaming) {
                lcdRow(0, "Place finger...");
                lcdRow(1, "Waiting sensor");
            } else {
                lcdRow(0, "Place finger...");
                lcdRow(1, "");
            }
            bufferIndex    = 0;
            validHeartRate = 0;
            validSpO2      = 0;
            particleSensor.nextSample();
            return;
        }

        // Store sample
        redBuffer[bufferIndex] = particleSensor.getRed();
        irBuffer[bufferIndex]  = irValue;
        particleSensor.nextSample();
        bufferIndex++;

        // Once buffer full → compute HR + SpO2
        if (bufferIndex >= BUFFER_SIZE) {
            maxim_heart_rate_and_oxygen_saturation(
                irBuffer,
                BUFFER_SIZE,
                redBuffer,
                &spo2,
                &validSpO2,
                &heartRate,
                &validHeartRate
            );

            // Only keep valid results
            if (validHeartRate && heartRate > 20 && heartRate < 250)
                latestHR = heartRate;
            if (validSpO2 && spo2 > 50 && spo2 <= 100)
                latestSpO2 = spo2;

            bufferIndex = 0;   // roll over buffer
        }
    }

    // ── Transmit on schedule (only when streaming active) ─────────────────────
    if (isStreaming && (millis() - lastSendTime > sendInterval)) {
        triggerReadAndTransmit();
        lastSendTime = millis();
    }

    // ── Idle LCD update every 1s (when not streaming) ─────────────────────────
    if (!isStreaming && (millis() - lastDisplayMs > 1000)) {
        lastDisplayMs = millis();

        float idleTemp = readTemperature();

        if (validHeartRate && validSpO2) {
            lcdRow(0, "HR:" + String(latestHR) + "bpm S:" + String(latestSpO2) + "%");
            lcdRow(1, "T:" + String(idleTemp, 1) + "\xDFC  Ready");
        } else {
            lcdRow(0, "Waiting...");
            lcdRow(1, "T:" + String(idleTemp, 1) + "\xDFC");
        }

        Serial.printf("[IDLE] HR:%d SpO2:%d Temp:%.1f\n", (int)latestHR, (int)latestSpO2, idleTemp);
    }
}
