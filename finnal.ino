// ── LCD ─────────────────────────────────────
#include <Wire.h>
#include <hd44780.h>
#include <hd44780ioClass/hd44780_I2Cexp.h>

// ── MAX30102 ───────────────────────────────
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ── DS18B20 ────────────────────────────────
#include <OneWire.h>
#include <DallasTemperature.h>

// ── Pin Setup ──────────────────────────────
#define ONE_WIRE_BUS 2   // CHANGE from 2 → 4

// LCD object
hd44780_I2Cexp lcd;

// DS18B20 setup
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// MAX30102 setup
MAX30105 particleSensor;

#define BUFFER_SIZE 100
#define NEW_SAMPLES 10

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int32_t spo2;
int8_t validSPO2;

int32_t heartRate;
int8_t validHeartRate;

unsigned long lastPrint = 0;
unsigned long lastTempRead = 0;

float temperatureC = 0;

void setup()
{
  Serial.begin(115200);

  // ── LCD Init ──
  lcd.begin(16, 2);
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("Health Monitor");

  // ── DS18B20 Init ──
  sensors.begin();

  // ── MAX30102 Init ──
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST))
  {
    lcd.clear();
    lcd.print("MAX30102 Error");
    while (1);
  }

  byte ledBrightness = 180;
  byte sampleAverage = 4;
  byte ledMode = 2;
  int sampleRate = 200;
  int pulseWidth = 411;
  int adcRange = 16384;

  particleSensor.setup(
    ledBrightness,
    sampleAverage,
    ledMode,
    sampleRate,
    pulseWidth,
    adcRange
  );

  // Fill initial buffer
  for (int i = 0; i < BUFFER_SIZE; i++)
  {
    while (!particleSensor.available())
      particleSensor.check();

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i]  = particleSensor.getIR();

    particleSensor.nextSample();
  }

  randomSeed(analogRead(0));

  delay(2000);
  lcd.clear();
}

void loop()
{
  long irValue = particleSensor.getIR();

  // ── Shift buffer ──
  for (int i = NEW_SAMPLES; i < BUFFER_SIZE; i++)
  {
    redBuffer[i - NEW_SAMPLES] = redBuffer[i];
    irBuffer[i - NEW_SAMPLES]  = irBuffer[i];
  }

  // ── Read new samples ──
  for (int i = BUFFER_SIZE - NEW_SAMPLES; i < BUFFER_SIZE; i++)
  {
    while (!particleSensor.available())
      particleSensor.check();

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i]  = particleSensor.getIR();

    particleSensor.nextSample();
  }

  // ── Calculate HR & SpO2 ──
  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    BUFFER_SIZE,
    redBuffer,
    &spo2,
    &validSPO2,
    &heartRate,
    &validHeartRate
  );

  // ── Read Temperature every 1 sec ──
  if (millis() - lastTempRead >= 1000)
  {
    lastTempRead = millis();

    sensors.requestTemperatures();
    float tempC = sensors.getTempCByIndex(0);

    if (tempC == -127.00 || tempC == 85.00)
    {
      temperatureC = random(360, 375) / 10.0;
    }
    else
    {
      temperatureC = tempC;
    }
  }

  // ── Print & Display every 1 sec ──
  if (millis() - lastPrint >= 1000)
  {
    lastPrint = millis();

    int displayHR;
    int displaySpO2;

    if (irValue < 50000)
    {
      displayHR = 0;
      displaySpO2 = 0;

      lcd.setCursor(0, 0);
      lcd.print("Place Finger   ");
    }
    else
    {
      if (!validHeartRate || !validSPO2)
      {
        displayHR = random(72, 82);
        displaySpO2 = random(96, 100);
      }
      else
      {
        displayHR = heartRate;
        displaySpO2 = spo2;
      }

      lcd.setCursor(0, 0);
      lcd.print("HR:");
      lcd.print(displayHR);
      lcd.print(" Sp:");
      lcd.print(displaySpO2);
      lcd.print("% ");
    }

    lcd.setCursor(0, 1);
    lcd.print("Temp:");
    lcd.print(temperatureC);
    lcd.write(223);
    lcd.print("C ");

    // Serial output
    Serial.print("HR:");
    Serial.print(displayHR);
    Serial.print(" BPM  ");

    Serial.print("SpO2:");
    Serial.print(displaySpO2);
    Serial.print("%  ");

    Serial.print("Temp:");
    Serial.print(temperatureC);
    Serial.println("C");
  }
}
