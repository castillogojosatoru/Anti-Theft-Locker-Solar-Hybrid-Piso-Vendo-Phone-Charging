#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>
#include <TM1637Display.h>
#include <Preferences.h>
#include <Adafruit_MCP23X17.h>  

/**
* ============================================================================
* Project: ESP32 Commercial Biometric Smart Charging Locker (Dual Compartment)
* Features: Admin Multi-Slot Memory Protection & Alternating Screen Display
* ============================================================================
*/

// ============================================================================
// HARDWARE CONFIGURATION & TOGGLES
// ============================================================================

const bool BENCH_TEST_MODE = true; 
const bool RELAY_ACTIVE_LOW = false;

Preferences prefs; 

// --- MCP23017 Expander Setup ---
Adafruit_MCP23X17 mcp;
#define MCP_I2C_ADDR     0x20
#define OUTLET_1_GPA_PIN 7    // GPA7 on MCP23017 (Outlet 1)
#define FAN_2_GPA_PIN    5    // GPA5 on MCP23017 (Fan 2)

// SW-420 Sensors mapped to MCP23017 Port B (B0 to B4)
const uint8_t SW420_PINS[5] = {8, 9, 10, 11, 12}; 
unsigned long lastTamperCheck = 0;
const unsigned long TAMPER_CHECK_INTERVAL_MS = 3000;

// Dedicated High-Decibel Alarm Buzzer Timer on GPIO 5
unsigned long alarmDeactivateTime = 0;
bool alarmActive = false;

// Non-blocking Tone Engine Variables
unsigned long toneEndTime = 0;
unsigned long toneHalfPeriodUs = 0;
unsigned long lastToneToggleUs = 0;
bool tonePinState = false;

// --- 1. Display Configuration ---
LiquidCrystal_I2C lcd(0x27, 16, 2);

#define CLK_1 18
#define DIO_1 4 
TM1637Display display1(CLK_1, DIO_1);

#define CLK_2 18
#define DIO_2 32
TM1637Display display2(CLK_2, DIO_2);

// --- Screen Cycling Control Variables ---
unsigned long lastScreenCycleTime = 0;
const unsigned long SCREEN_CYCLE_INTERVAL_MS = 3000; // Alternates every 3 seconds
bool showCoinPromptToggle = false;

// --- 2. User Button Pins ---
const int PIN_BTN_1     = 25;  
const int PIN_BTN_2     = 19;  
const int PIN_BTN_BLUE  = 33;  
const int PIN_BTN_RED   = 26;  
const int PIN_BTN_GREEN = 27;  

// --- 3. Solenoid, Relay, Fan & Alarm Pins ---
const int PIN_SOLENOID_1   = 14; 
const int PIN_SOLENOID_2   = 12; 

const int PIN_OUTLET_2     = 13; 
const int PIN_FAN_1        = 15; 
const int PIN_ALARM_BUZZER = 5; 

const unsigned long SOLENOID_PULSE_DURATION_MS = 5000;
unsigned long solenoidDeactivateTime[3] = {0, 0, 0};
bool          solenoidActive[3]         = {false, false, false};

volatile bool solenoidsActiveNow = false;

// --- 4. Coin Slot Configuration & Balanced Noise Immunity ---
const uint8_t COIN_PIN               = 23;    
const uint8_t LED_PIN                = 2;     
const bool    INSTANT_MODE           = false; 

const unsigned long DEBOUNCE_DELAY_MS = 45;   
const unsigned long COIN_TIMEOUT_MS   = 400;  

// Transient Noise Muting Timestamp
volatile unsigned long ignoreCoinInterruptsUntil = 0;

// --- 5. Charging Time Tiers ---
const uint32_t TIME_1_PESO_MIN   = 5;    
const uint32_t TIME_2_PESOS_MIN  = 8;    
const uint32_t TIME_5_PESOS_MIN  = 20;   
const uint32_t TIME_10_PESOS_MIN = 45;   
const uint32_t TIME_15_PESOS_MIN = 70;   
const uint32_t TIME_20_PESOS_MIN = 100;  

// --- 6. Fingerprint Sensor Configuration ---
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

// --- 7. Interrupt Variables ---
volatile unsigned long lastPulseTimeUs  = 0; 
volatile uint16_t      isrPulseCount    = 0;
volatile bool          newPulseDetected = false;

uint16_t accumulatedPulses    = 0;
uint32_t totalCoinsInserted   = 0;
uint32_t totalPesos           = 0; 
uint32_t totalChargingMinutes = 0; 

// --- 8. Compartment State Machine ---
struct WindowState {
  bool isLocked;
  bool isRegistered;
  bool isCharging;
  unsigned long lockStartTime;
  unsigned long chargeStartTime;
  unsigned long durationSec;  
};

WindowState windows[3] = {
  {false, false, false, 0, 0, 0}, 
  {false, false, false, 0, 0, 0}, 
  {false, false, false, 0, 0, 0}  
};

// --- 9. UI Input State & Screen Timers ---
enum ActionType { NONE, ACTION_OPEN, ACTION_LOCK, ACTION_START_CHARGING };
ActionType pendingAction = NONE;
int selectedWindow       = 0; 

// --- Admin Subsystem States ---
bool adminModeActive = false;
int adminTargetLocker = 0;

unsigned long screenResetTime       = 0;
bool          temporaryScreenActive = false;

unsigned long lastSerialTimerReport = 0;
const unsigned long SERIAL_TIMER_INTERVAL_MS = 1000;

unsigned long lastDisplay1Update = 0;
unsigned long lastDisplay2Update = 0;

// Sound Feedback Enum Types
enum SoundType {
  BUZZ_SELECT,
  BUZZ_COIN,
  BUZZ_SUCCESS,
  BUZZ_TRY_AGAIN,
  BUZZ_ERROR
};

// --- Helper Declarations ---
void writeRelay(int pin, bool activate, bool isMCP = false) {
  if (pin < 0) return;
  bool state = RELAY_ACTIVE_LOW ? !activate : activate;
  
  if (isMCP) {
    mcp.digitalWrite(pin, state ? HIGH : LOW);
  } else {
    digitalWrite(pin, state ? HIGH : LOW);
  }
}

void muteCoinInterrupts(unsigned long durationMs) {
  ignoreCoinInterruptsUntil = millis() + durationMs;
}

void writeFan(int fanNum, bool activate) {
  if (fanNum == 1) {
    digitalWrite(PIN_FAN_1, activate ? HIGH : LOW);
  } else if (fanNum == 2) {
    mcp.digitalWrite(FAN_2_GPA_PIN, activate ? LOW : HIGH);
  }
}

void resetUIState() {
  selectedWindow = 0;
  pendingAction = NONE;
  adminModeActive = false;
  adminTargetLocker = 0;
}

// ============================================================================
// NON-BLOCKING BUZZER SUBSYSTEM
// ============================================================================
void startTone(uint16_t frequencyHz, uint16_t durationMs) {
  if (alarmActive) return; 
  if (frequencyHz == 0) {
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    toneEndTime = 0;
    return;
  }

  toneHalfPeriodUs = 1000000UL / (frequencyHz * 2);
  toneEndTime = millis() + durationMs;
  lastToneToggleUs = micros();
  tonePinState = true;
  digitalWrite(PIN_ALARM_BUZZER, HIGH);
}

void updateToneEngine() {
  if (alarmActive) return; 
  if (toneEndTime == 0) return;

  if (millis() >= toneEndTime) {
    toneEndTime = 0;
    tonePinState = false;
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    return;
  }

  if (micros() - lastToneToggleUs >= toneHalfPeriodUs) {
    lastToneToggleUs = micros();
    tonePinState = !tonePinState;
    digitalWrite(PIN_ALARM_BUZZER, tonePinState ? HIGH : LOW);
  }
}

void triggerBuzzer(SoundType type) {
  switch (type) {
    case BUZZ_SELECT:    startTone(2400, 15);  break;
    case BUZZ_COIN:      startTone(2500, 80);  break;
    case BUZZ_SUCCESS:   startTone(1500, 120); break;
    case BUZZ_TRY_AGAIN: startTone(1000, 80);  break;
    case BUZZ_ERROR:     startTone(1200, 80);  break;
  }
}

// --- 10. Button Debounce Struct ---
struct Button {
  const int pin;
  const char* name;
  int lastReading;
  int stableState;
  unsigned long lastDebounceTime;

  Button(int p, const char* n) : pin(p), name(n), lastReading(HIGH), stableState(HIGH), lastDebounceTime(0) {}

  void init() {
    pinMode(pin, INPUT_PULLUP);
  }

  bool isPressedRaw() {
    return digitalRead(pin) == LOW;
  }

  bool wasPressed() {
    int reading = digitalRead(pin);
    bool pressed = false;

    if (reading != lastReading) {
      lastDebounceTime = millis();
    }

    if ((millis() - lastDebounceTime) > 20) { 
      if (reading != stableState) {
        stableState = reading;
        if (stableState == LOW) {
          pressed = true;
          Serial.printf("[BUTTON EVENT] %s (GPIO %d) CLICKED\n", name, pin);
        }
      }
    }
    lastReading = reading;
    return pressed;
  }
};

Button btn1(PIN_BTN_1, "WIN1_BTN");
Button btn2(PIN_BTN_2, "WIN2_BTN");
Button btnBlue(PIN_BTN_BLUE, "BLUE_OPEN_BTN");
Button btnRed(PIN_BTN_RED, "RED_LOCK_BTN");
Button btnGreen(PIN_BTN_GREEN, "GREEN_CHARGE_BTN");

// Forward Declarations
void showScreen(const char* line1, const char* line2);
void triggerTemporaryMessage(const char* line1, const char* line2, unsigned long durationMs);
void triggerTemporaryMessage(const char* line1, const char* line2);
void showIdleScreen();
bool isDoorClosed(int window);
void triggerUnlockPulse(int window);
void updateSolenoidPulses();
void updateChargingOutlets();
uint32_t calculateChargingMinutes(uint16_t pulses);
void processDetectedCoin(uint16_t pulses);
bool registerFingerprintForWindow(int windowId);
bool verifyFingerprintForWindow(int expectedWindowId);
void formatTime(unsigned long totalSeconds, char* buffer, size_t bufSize);
void printBackgroundTimerToSerial();
void executeAction(int window, ActionType action);
void updateTM1637Timer1();
void updateTM1637Timer2();
void checkTamperSensors();
void updateAlarmState();
void handleRedButton();
void handleGreenButton();
void registerAdminFingerprints();
void processAdminScanAndOverride();
void processAdminTimerReset();

// ============================================================================
// HARDWARE SUBSYSTEM HELPERS & TAMPER MONITORING
// ============================================================================
void updateAlarmState() {
  if (alarmActive && millis() >= alarmDeactivateTime) {
    digitalWrite(PIN_ALARM_BUZZER, LOW);
    alarmActive = false;
    Serial.println(F("[ALERT] GPIO 5 Alarm Buzzer De-energized."));
  }
}

void checkTamperSensors() {
  if (solenoidsActiveNow) return; 

  if (millis() - lastTamperCheck < TAMPER_CHECK_INTERVAL_MS) return;
  lastTamperCheck = millis();

  bool triggered = false;
  for (int i = 0; i < 5; i++) {
    if (mcp.digitalRead(SW420_PINS[i]) == HIGH) {
      triggered = true;
      break;
    }
  }

  if (triggered) {
    Serial.println(F("[ALERT] Tampering detected on SW-420 Sensors!"));
    
    digitalWrite(PIN_ALARM_BUZZER, HIGH);
    alarmActive = true;
    alarmDeactivateTime = millis() + 10000;

    triggerBuzzer(BUZZ_ERROR);
    triggerTemporaryMessage("TAMPER ALERT!", "VIBRATION DETECT", 2000);
  }
}

bool isDoorClosed(int window) {
  return true; 
}

void triggerUnlockPulse(int window) {
  if (window < 1 || window > 2) return;
  
  muteCoinInterrupts(250);

  solenoidsActiveNow = true;

  int pin = (window == 1) ? PIN_SOLENOID_1 : PIN_SOLENOID_2;
  writeRelay(pin, true);
  solenoidDeactivateTime[window] = millis() + SOLENOID_PULSE_DURATION_MS;
  solenoidActive[window] = true;
  Serial.printf("[Solenoid] Window %d ENERGIZED (%lu ms).\n", window, SOLENOID_PULSE_DURATION_MS);
}

void updateSolenoidPulses() {
  bool activeFound = false;

  for (int i = 1; i <= 2; i++) {
    if (solenoidActive[i]) {
      if (millis() >= solenoidDeactivateTime[i]) {
        int pin = (i == 1) ? PIN_SOLENOID_1 : PIN_SOLENOID_2;
        
        muteCoinInterrupts(150);
        
        writeRelay(pin, false);
        solenoidActive[i] = false;
        Serial.printf("[Solenoid] Window %d DE-ENERGIZED.\n", i);
      } else {
        activeFound = true;
      }
    }
  }

  solenoidsActiveNow = activeFound;
}

void updateChargingOutlets() {
  for (int i = 1; i <= 2; i++) {
    bool isMCP = (i == 1);
    int outletPin = (i == 1) ? OUTLET_1_GPA_PIN : PIN_OUTLET_2;

    if (windows[i].isCharging) {
      unsigned long elapsedSec = (millis() - windows[i].chargeStartTime) / 1000;
      if (elapsedSec < windows[i].durationSec) {
        writeRelay(outletPin, true, isMCP); 
      } else {
        windows[i].isCharging = false;
        windows[i].durationSec = 0;
        windows[i].chargeStartTime = 0;
        writeRelay(outletPin, false, isMCP); 
        writeFan(i, false); 
        
        Serial.printf("[Power Outlet] Timer expired for Window %d. Power & Fan OFF.\n", i);
      }
    } else {
      writeRelay(outletPin, false, isMCP);
      if (!windows[i].isLocked) {
        writeFan(i, false); 
      }
    }
  }
}

uint32_t calculateChargingMinutes(uint16_t pulses) {
  switch (pulses) {
    case 1:  return TIME_1_PESO_MIN;
    case 2:  return TIME_2_PESOS_MIN;
    case 5:  return TIME_5_PESOS_MIN;
    case 10: return TIME_10_PESOS_MIN;
    case 15: return TIME_15_PESOS_MIN;
    case 20: return TIME_20_PESOS_MIN;
    default: break;
  }

  uint32_t minutes = 0;
  uint16_t remaining = pulses;

  if (remaining >= 20) {
    minutes += (remaining / 20) * TIME_20_PESOS_MIN;
    remaining %= 20;
  }
  if (remaining >= 15) {
    minutes += TIME_15_PESOS_MIN;
    remaining -= 15;
  }
  if (remaining >= 10) {
    minutes += TIME_10_PESOS_MIN;
    remaining -= 10;
  }
  if (remaining >= 5) {
    minutes += TIME_5_PESOS_MIN;
    remaining -= 5;
  }
  if (remaining >= 2) {
    minutes += (remaining / 2) * TIME_2_PESOS_MIN;
    remaining %= 2;
  }
  if (remaining == 1) {
    minutes += TIME_1_PESO_MIN;
    remaining = 0;
  }

  return minutes;
}

// ============================================================================
// TM1637 TIMER DISPLAYS
// ============================================================================
void updateTM1637Timer1() {
  if (millis() - lastDisplay1Update < 1000) return;
  lastDisplay1Update = millis();

  if (windows[1].isCharging) {
    unsigned long elapsedSec = (millis() - windows[1].chargeStartTime) / 1000;
    
    if (elapsedSec < windows[1].durationSec) {
      unsigned long remSec = windows[1].durationSec - elapsedSec;
      
      uint8_t displayMinutes = remSec / 60;
      uint8_t displaySeconds = remSec % 60;

      bool showColon = (remSec % 2 == 0);
      display1.showNumberDecEx((displayMinutes * 100) + displaySeconds, showColon ? 0b01000000 : 0, true, 4, 0);
    } else {
      display1.showNumberDecEx(0, 0b01000000, true, 4, 0);
    }
  } else if (windows[1].durationSec > 0) {
    uint8_t displayMinutes = windows[1].durationSec / 60;
    display1.showNumberDecEx((displayMinutes * 100), 0b01000000, true, 4, 0);
  } else {
    display1.showNumberDecEx(0, 0b01000000, true, 4, 0);
  }
}

void updateTM1637Timer2() {
  if (millis() - lastDisplay2Update < 1000) return;
  lastDisplay2Update = millis();

  if (windows[2].isCharging) {
    unsigned long elapsedSec = (millis() - windows[2].chargeStartTime) / 1000;
    
    if (elapsedSec < windows[2].durationSec) {
      unsigned long remSec = windows[2].durationSec - elapsedSec;
      
      uint8_t displayMinutes = remSec / 60;
      uint8_t displaySeconds = remSec % 60;

      bool showColon = (remSec % 2 == 0);
      display2.showNumberDecEx((displayMinutes * 100) + displaySeconds, showColon ? 0b01000000 : 0, true, 4, 0);
    } else {
      display2.showNumberDecEx(0, 0b01000000, true, 4, 0);
    }
  } else if (windows[2].durationSec > 0) {
    uint8_t displayMinutes = windows[2].durationSec / 60;
    display2.showNumberDecEx((displayMinutes * 100), 0b01000000, true, 4, 0);
  } else {
    display2.showNumberDecEx(0, 0b01000000, true, 4, 0);
  }
}

// ============================================================================
// INTERRUPT SERVICE ROUTINE (STRICT DUAL-FILTERED COIN PULSE)
// ============================================================================
void IRAM_ATTR coinPulseISR() {
  if (millis() < ignoreCoinInterruptsUntil) return;

  unsigned long currentMicros = micros();
  
  if (currentMicros - lastPulseTimeUs >= (DEBOUNCE_DELAY_MS * 1000UL)) {
    if (digitalRead(COIN_PIN) == LOW) {
      isrPulseCount++;
      lastPulseTimeUs = currentMicros;
      newPulseDetected = true;
    }
  }
}

void processDetectedCoin(uint16_t pulses) {
  if (pulses == 0) return;

  uint16_t coinValue    = pulses; 
  uint32_t addedMinutes = calculateChargingMinutes(pulses);

  totalCoinsInserted++;
  totalPesos += coinValue;
  totalChargingMinutes += addedMinutes;

  triggerBuzzer(BUZZ_COIN);

  Serial.println(F("\n====== [COIN DETECTED] ======"));
  Serial.printf("Pulses Counted : %u\n", pulses);
  Serial.printf("Coin Value     : PHP %u\n", coinValue);
  Serial.printf("Charging Added : +%u Minutes\n", addedMinutes);
  Serial.printf("Total Pool     : PHP %u (%u Minutes Available)\n", totalPesos, totalChargingMinutes);
  Serial.println(F("============================="));

  char line1[17], line2[17];
  snprintf(line1, sizeof(line1), "+%uP (+%um CHG)", coinValue, addedMinutes);
  snprintf(line2, sizeof(line2), "BAL:%uP TIME:%um", totalPesos, totalChargingMinutes);
  triggerTemporaryMessage(line1, line2, 1600);
}

// ============================================================================
// DISPLAY MANAGEMENT
// ============================================================================
void showScreen(const char* line1, const char* line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void triggerTemporaryMessage(const char* line1, const char* line2, unsigned long durationMs) {
  showScreen(line1, line2);
  temporaryScreenActive = true;
  screenResetTime = millis() + durationMs;
  resetUIState(); 
}

void triggerTemporaryMessage(const char* line1, const char* line2) {
  triggerTemporaryMessage(line1, line2, 1500);
}

void showIdleScreen() {
  if (adminModeActive) {
    char l1[17];
    snprintf(l1, sizeof(l1), "ADMIN MODE: WIN%d", adminTargetLocker);
    showScreen(l1, "SCAN ADMIN PRINT");
  } else if (pendingAction == ACTION_OPEN) {
    showScreen("UNLOCK SELECTED", "CHOOSE LOCKER 1/2");
  } else if (pendingAction == ACTION_LOCK) {
    showScreen("LOCK SELECTED", "CHOOSE LOCKER 1/2");
  } else if (pendingAction == ACTION_START_CHARGING) {
    showScreen("START CHARGING", "CHOOSE LOCKER 1/2");
  } else if (selectedWindow != 0) {
    char l1[17];
    snprintf(l1, sizeof(l1), "WIN %d SELECTED", selectedWindow);
    showScreen(l1, "RED:LOCK GRN:CHG");
  } else {
    // --- Line 1 Alternator Logic ---
    char l1[17];
    if (showCoinPromptToggle) {
      snprintf(l1, sizeof(l1), "PLS INSERT COIN ");
    } else {
      snprintf(l1, sizeof(l1), "BAL:%uP | %um CHG", totalPesos, totalChargingMinutes);
    }

    // --- Line 2 Status Logic ---
    char w1Tag[8] = "IDLE";
    char w2Tag[8] = "IDLE";

    for (int i = 1; i <= 2; i++) {
      if (windows[i].isCharging) {
        snprintf(i == 1 ? w1Tag : w2Tag, sizeof(w1Tag), "CHRG");
      } else if (windows[i].durationSec > 0) {
        snprintf(i == 1 ? w1Tag : w2Tag, sizeof(w1Tag), "CHRG");
      } else if (windows[i].isLocked) {
        snprintf(i == 1 ? w1Tag : w2Tag, sizeof(w1Tag), "LCKD");
      }
    }

    char l2[17];
    snprintf(l2, sizeof(l2), "W1:%-4s W2:%-4s", w1Tag, w2Tag);
    showScreen(l1, l2);
  }
}

// ============================================================================
// BIOMETRIC SECURITY SUBSYSTEM & ADMIN PRE-REGISTRATION
// ============================================================================
bool registerFingerprintForWindow(int windowId) {
  int p = -1;
  unsigned long timeout;

  Serial.printf("\n[Biometrics] Enrolling Template into Slot #%d...\n", windowId);

  showScreen("PLACE FINGER", "FOR REGISTRATION");
  timeout = millis() + 10000;
  while (p != FINGERPRINT_OK) {
    if (millis() > timeout) return false;
    p = finger.getImage();
    delay(50);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) return false;

  showScreen("REMOVE FINGER", "PLEASE WAIT...");
  delay(1000);
  p = 0;
  while (p != FINGERPRINT_NOFINGER) {
    p = finger.getImage();
    delay(50);
  }

  showScreen("PLACE AGAIN", "VERIFYING PRINT");
  timeout = millis() + 10000;
  p = -1;
  while (p != FINGERPRINT_OK) {
    if (millis() > timeout) return false;
    p = finger.getImage();
    delay(50);
  }

  p = finger.image2Tz(2);
  if (p != FINGERPRINT_OK) return false;

  p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    triggerBuzzer(BUZZ_TRY_AGAIN);
    triggerTemporaryMessage("PRINTS MISMATCH", "TRY AGAIN", 1500);
    return false;
  }

  p = finger.storeModel(windowId);
  if (p == FINGERPRINT_OK) {
    triggerBuzzer(BUZZ_SUCCESS);
    Serial.printf("[Biometrics] Stored ID #%d to AS608 Flash.\n", windowId);
    return true;
  }
  
  return false;
}

void registerAdminFingerprints() {
  int adminSlots[3] = {3, 4, 5};

  for (int i = 0; i < 3; i++) {
    int slot = adminSlots[i];

    if (finger.loadModel(slot) == FINGERPRINT_OK) {
      Serial.printf("[Biometrics] Admin Slot #%d present in AS608 Flash.\n", slot);
      continue;
    }

    bool enrolled = false;
    while (!enrolled) {
      char l1[17], l2[17];
      snprintf(l1, sizeof(l1), "ENROLL ADMIN #%d", slot);
      snprintf(l2, sizeof(l2), "PLACE FINGER NOW");
      showScreen(l1, l2);
      triggerBuzzer(BUZZ_SELECT);

      enrolled = registerFingerprintForWindow(slot);
      if (!enrolled) {
        showScreen("ENROLL FAILED", "RETRYING ADMIN...");
        delay(1500);
      }
    }

    char msg[17];
    snprintf(msg, sizeof(msg), "ADMIN #%d SAVED!", slot);
    triggerTemporaryMessage("ADMIN SUCCESS", msg, 1500);
  }
}

bool verifyFingerprintForWindow(int expectedWindowId) {
  int p = -1;
  unsigned long timeout = millis() + 8000;

  showScreen("SCAN FINGERPRINT", "TO UNLOCK WINDOW");

  while (p != FINGERPRINT_OK) {
    if (millis() > timeout) return false;
    p = finger.getImage();
    delay(50);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) return false;

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK && finger.fingerID == expectedWindowId) {
    triggerBuzzer(BUZZ_SUCCESS);
    return true;
  }

  return false;
}

void processAdminScanAndOverride() {
  showScreen("SCAN ADMIN PRINT", "OVERRIDE LOCKER");

  int p = -1;
  unsigned long timeout = millis() + 8000;

  while (p != FINGERPRINT_OK) {
    if ((btn1.isPressedRaw() || btn2.isPressedRaw()) && btnRed.isPressedRaw()) {
      triggerBuzzer(BUZZ_ERROR);
      triggerTemporaryMessage("ADMIN OVERRIDE", "CANCELLED / EXIT", 1200);
      return;
    }

    if (millis() > timeout) {
      triggerBuzzer(BUZZ_ERROR);
      triggerTemporaryMessage("ADMIN TIMEOUT", "OVERRIDE ABORTED", 1500);
      return;
    }

    p = finger.getImage();
    delay(50);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    triggerBuzzer(BUZZ_ERROR);
    triggerTemporaryMessage("ADMIN ERROR", "IMAGE INVALID", 1500);
    return;
  }

  p = finger.fingerSearch();

  if (p == FINGERPRINT_OK && (finger.fingerID == 3 || finger.fingerID == 4 || finger.fingerID == 5)) {
    triggerBuzzer(BUZZ_SUCCESS);

    int target = adminTargetLocker;

    finger.deleteModel(target);

    windows[target].isLocked     = false;
    windows[target].isRegistered = false;

    windows[target].isCharging      = false;
    windows[target].durationSec     = 0;
    windows[target].chargeStartTime = 0;

    bool isMCP = (target == 1);
    int outletPin = (target == 1) ? OUTLET_1_GPA_PIN : PIN_OUTLET_2;
    writeRelay(outletPin, false, isMCP); 
    writeFan(target, false);            

    triggerUnlockPulse(target);

    char line1[17];
    snprintf(line1, sizeof(line1), "ADMIN OVERRIDE #%d", finger.fingerID);
    triggerTemporaryMessage(line1, "RESET & UNLOCKED", 2200);
  } 
  else if (p == FINGERPRINT_OK && (finger.fingerID == 1 || finger.fingerID == 2)) {
    triggerBuzzer(BUZZ_ERROR);
    char errLine[17];
    snprintf(errLine, sizeof(errLine), "USER ID #%d MATCHED", finger.fingerID);
    triggerTemporaryMessage("NOT ADMIN PRINT!", errLine, 2200);
  } 
  else {
    triggerBuzzer(BUZZ_ERROR);
    triggerTemporaryMessage("ACCESS DENIED!", "NO ADMIN MATCH", 2000);
  }

  resetUIState();
}

void processAdminTimerReset() {
  int target = adminTargetLocker;
  char headerMsg[17];
  snprintf(headerMsg, sizeof(headerMsg), "RESET WIN%d TIMER", target);
  showScreen(headerMsg, "SCAN ADMIN PRINT");

  int p = -1;
  unsigned long timeout = millis() + 8000;

  while (p != FINGERPRINT_OK) {
    if ((btn1.isPressedRaw() || btn2.isPressedRaw()) && btnGreen.isPressedRaw()) {
      triggerBuzzer(BUZZ_ERROR);
      triggerTemporaryMessage("TIMER RESET", "CANCELLED / EXIT", 1200);
      return;
    }

    if (millis() > timeout) {
      triggerBuzzer(BUZZ_ERROR);
      triggerTemporaryMessage("ADMIN TIMEOUT", "RESET ABORTED", 1500);
      return;
    }

    p = finger.getImage();
    delay(50);
  }

  p = finger.image2Tz(1);
  if (p != FINGERPRINT_OK) {
    triggerBuzzer(BUZZ_ERROR);
    triggerTemporaryMessage("ADMIN ERROR", "IMAGE INVALID", 1500);
    return;
  }

  p = finger.fingerSearch();

  if (p == FINGERPRINT_OK && (finger.fingerID == 3 || finger.fingerID == 4 || finger.fingerID == 5)) {
    triggerBuzzer(BUZZ_SUCCESS);

    windows[target].isCharging      = false;
    windows[target].durationSec     = 0;
    windows[target].chargeStartTime = 0;

    bool isMCP = (target == 1);
    int outletPin = (target == 1) ? OUTLET_1_GPA_PIN : PIN_OUTLET_2;
    writeRelay(outletPin, false, isMCP); 
    
    if (!windows[target].isLocked) {
      writeFan(target, false);            
    }

    char line1[17];
    snprintf(line1, sizeof(line1), "WIN%d TIMER CLEARED", target);
    triggerTemporaryMessage(line1, "POWER CUT OFF", 2200);
  } 
  else if (p == FINGERPRINT_OK && (finger.fingerID == 1 || finger.fingerID == 2)) {
    triggerBuzzer(BUZZ_ERROR);
    char errLine[17];
    snprintf(errLine, sizeof(errLine), "USER ID #%d MATCHED", finger.fingerID);
    triggerTemporaryMessage("NOT ADMIN PRINT!", errLine, 2200);
  } 
  else {
    triggerBuzzer(BUZZ_ERROR);
    triggerTemporaryMessage("ACCESS DENIED!", "NO ADMIN MATCH", 2000);
  }

  resetUIState();
}

// ============================================================================
// BACKGROUND TIMER & SERIAL MONITOR REPORTING
// ============================================================================
void formatTime(unsigned long totalSeconds, char* buffer, size_t bufSize) {
  unsigned int hours   = totalSeconds / 3600;
  unsigned int minutes = (totalSeconds % 3600) / 60;
  unsigned int seconds = totalSeconds % 60;
  snprintf(buffer, bufSize, "%02u:%02u:%02u", hours, minutes, seconds);
}

void printBackgroundTimerToSerial() {
  char w1Report[36];
  char w2Report[36];

  for (int i = 1; i <= 2; i++) {
    char* targetBuf = (i == 1) ? w1Report : w2Report;
    size_t bufLen   = sizeof(w1Report);

    if (windows[i].isCharging) {
      unsigned long elapsedSec = (millis() - windows[i].chargeStartTime) / 1000;
      char elapsedStr[10];
      formatTime(elapsedSec, elapsedStr, sizeof(elapsedStr));

      if (elapsedSec < windows[i].durationSec) {
        unsigned long remSec = windows[i].durationSec - elapsedSec;
        char remStr[10];
        formatTime(remSec, remStr, sizeof(remStr)); 
        snprintf(targetBuf, bufLen, "CHARGING Rem:%s (Elap:%s)", remStr, elapsedStr);
      } else {
        snprintf(targetBuf, bufLen, "CHARGING DONE (Elap:%s)", elapsedStr);
      }
    } else if (windows[i].durationSec > 0) {
      snprintf(targetBuf, bufLen, "TIMER SET (%lu min, UNSTARTED)", windows[i].durationSec / 60);
    } else if (windows[i].isLocked) {
      snprintf(targetBuf, bufLen, "LOCKED (OFF)");
    } else {
      snprintf(targetBuf, bufLen, "OPEN / IDLE");
    }
  }

  Serial.printf("[LOCKER LOG] Win1: [%s] | Win2: [%s] | Credit Pool: PHP %u (%u min unassigned)\n",
                w1Report, w2Report, totalPesos, totalChargingMinutes);
}

// ============================================================================
// SYSTEM ACTION EXECUTION ENGINE
// ============================================================================
void executeAction(int window, ActionType action) {
  char l1[17], l2[17];

  // --- RED BUTTON: LOCK LOCKER & START TIMER ---
  if (action == ACTION_LOCK) {
    if (window != 1 && window != 2) {
      triggerBuzzer(BUZZ_ERROR);
      triggerTemporaryMessage("INVALID WINDOW", "SELECT AGAIN", 2000);
      return;
    }

    if (windows[window].isRegistered || windows[window].isLocked) {
      triggerBuzzer(BUZZ_ERROR);
      snprintf(l1, sizeof(l1), "LOCKER %d UNAVAIL", window);
      triggerTemporaryMessage(l1, "FINGERPRINT STORED", 2000);
      return;
    }

    if (!isDoorClosed(window)) {
      triggerBuzzer(BUZZ_ERROR);
      triggerTemporaryMessage("ERROR: CLOSE DOOR", "BEFORE LOCKING", 2000);
      return;
    }

    bool enrolled = registerFingerprintForWindow(window);

    if (enrolled) {
      windows[window].lockStartTime = millis();
      windows[window].isLocked      = true;
      windows[window].isRegistered  = true;

      if (windows[window].durationSec > 0) {
        windows[window].isCharging      = true;
        windows[window].chargeStartTime = millis();

        bool isMCP = (window == 1);
        int outletPin = (window == 1) ? OUTLET_1_GPA_PIN : PIN_OUTLET_2;
        writeRelay(outletPin, true, isMCP);
      }

      triggerUnlockPulse(window); 
      writeFan(window, true);

      snprintf(l1, sizeof(l1), "WIN %d: LOCKED", window);
      snprintf(l2, sizeof(l2), windows[window].isCharging ? "TIMER STARTED!" : "FINGERPRINT SAVED");
      triggerTemporaryMessage(l1, l2, 2000);
    } else {
      triggerTemporaryMessage("REGISTRATION ERR", "LOCKING ABORTED", 2000);
    }
  }

  // --- GREEN BUTTON: PRESET CHARGING TIME ---
  else if (action == ACTION_START_CHARGING) {
    if (totalChargingMinutes == 0) {
      triggerBuzzer(BUZZ_ERROR);
      triggerTemporaryMessage("INSERT A COIN 1st", "NO CREDITS SAVED", 2000);
      return;
    }

    uint32_t assignedMinutes = totalChargingMinutes;

    windows[window].durationSec = assignedMinutes * 60UL;
    windows[window].isCharging  = false; 

    totalPesos = 0;
    totalChargingMinutes = 0;

    triggerBuzzer(BUZZ_SUCCESS);
    snprintf(l1, sizeof(l1), "WIN %d TIMER SET!", window);
    snprintf(l2, sizeof(l2), "PRESS RED TO LOCK");
    triggerTemporaryMessage(l1, l2, 2000);
  }

  // --- BLUE BUTTON: UNLOCK LOCKER ---
  else if (action == ACTION_OPEN) {
    if (!windows[window].isRegistered) {
      triggerBuzzer(BUZZ_SUCCESS);
      triggerUnlockPulse(window);
      
      windows[window].isLocked = false;
      writeFan(window, false);

      snprintf(l1, sizeof(l1), "NO PRINT STORED");
      snprintf(l2, sizeof(l2), "UNLOCKING WIN %d", window);
      triggerTemporaryMessage(l1, l2, 2000);
      return;
    }

    bool verified = verifyFingerprintForWindow(window);

    if (verified) {
      triggerUnlockPulse(window);

      windows[window].isLocked     = false;
      windows[window].isRegistered = false;
      finger.deleteModel(window);

      writeFan(window, false);

      snprintf(l1, sizeof(l1), "ACCESS GRANTED!");
      snprintf(l2, sizeof(l2), "WIN %d UNLOCKED", window);
      triggerTemporaryMessage(l1, l2, 2000);
    } else {
      triggerTemporaryMessage("ACCESS DENIED!", "WRONG FINGERPRINT", 2200);
    }
  }
}

// ============================================================================
// BUTTON HANDLERS
// ============================================================================
void handleRedButton() {
  if (selectedWindow != 1 && selectedWindow != 2) {
    pendingAction = ACTION_LOCK;
    showIdleScreen();
    return;
  }

  pendingAction = ACTION_LOCK;
  executeAction(selectedWindow, pendingAction);
}

void handleGreenButton() {
  if (totalChargingMinutes == 0) {
    triggerBuzzer(BUZZ_ERROR);
    triggerTemporaryMessage("INSERT COIN FIRST", "NO CREDITS STORED", 2000);
    return;
  }

  if (selectedWindow != 1 && selectedWindow != 2) {
    pendingAction = ACTION_START_CHARGING;
    showIdleScreen();
    return;
  }

  pendingAction = ACTION_START_CHARGING;
  executeAction(selectedWindow, pendingAction);
}

// ============================================================================
// ARDUINO SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(PIN_SOLENOID_1, OUTPUT);
  pinMode(PIN_SOLENOID_2, OUTPUT);
  pinMode(PIN_OUTLET_2, OUTPUT);
  pinMode(PIN_ALARM_BUZZER, OUTPUT);
  pinMode(PIN_FAN_1, OUTPUT);

  writeRelay(PIN_SOLENOID_1, false);
  writeRelay(PIN_SOLENOID_2, false);
  writeRelay(PIN_OUTLET_2, false);
  digitalWrite(PIN_ALARM_BUZZER, LOW);
  digitalWrite(PIN_FAN_1, LOW);

  display1.setBrightness(0x0f); 
  display1.showNumberDecEx(0, 0b01000000, true, 4, 0);

  display2.setBrightness(0x0f); 
  display2.showNumberDecEx(0, 0b01000000, true, 4, 0);

  Wire.begin(21, 22);
  Wire.setClock(100000); 
  lcd.init();
  lcd.backlight();
  showScreen("SYSTEM BOOTING", "PLEASE WAIT...");

  if (!mcp.begin_I2C(MCP_I2C_ADDR)) {
    Serial.println(F("[MCP23017] Error: Expander initialization failed!"));
  } else {
    Serial.println(F("[MCP23017] Board detected."));
    
    mcp.pinMode(OUTLET_1_GPA_PIN, OUTPUT);
    writeRelay(OUTLET_1_GPA_PIN, false, true);
    
    mcp.pinMode(FAN_2_GPA_PIN, OUTPUT);
    mcp.digitalWrite(FAN_2_GPA_PIN, HIGH); 

    for (int i = 0; i < 5; i++) {
      mcp.pinMode(SW420_PINS[i], INPUT);
    }
  }

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(COIN_PIN, INPUT_PULLUP);

  fingerSerial.begin(57600, SERIAL_8N1, 16, 17);
  finger.begin(57600);

  btn1.init();
  btn2.init();
  btnBlue.init();
  btnRed.init();
  btnGreen.init();

  registerAdminFingerprints();

  triggerBuzzer(BUZZ_SUCCESS);
  resetUIState();
  showIdleScreen();

  delay(100); 

  noInterrupts();
  isrPulseCount = 0;
  newPulseDetected = false;
  lastPulseTimeUs = micros();
  interrupts();

  attachInterrupt(digitalPinToInterrupt(COIN_PIN), coinPulseISR, FALLING);
}

// ============================================================================
// ARDUINO MAIN LOOP
// ============================================================================
void loop() {
  updateToneEngine();      
  updateSolenoidPulses();
  updateChargingOutlets();
  checkTamperSensors();    
  updateAlarmState();      
  
  updateTM1637Timer1();
  updateTM1637Timer2();

  // --- NON-BLOCKING SCREEN ALTERNATOR ---
  if (millis() - lastScreenCycleTime >= SCREEN_CYCLE_INTERVAL_MS) {
    lastScreenCycleTime = millis();
    showCoinPromptToggle = !showCoinPromptToggle;
    
    if (!temporaryScreenActive) {
      showIdleScreen();
    }
  }

  if (temporaryScreenActive && millis() >= screenResetTime) {
    temporaryScreenActive = false;
    resetUIState();
    showIdleScreen();
  }

  if (millis() - lastSerialTimerReport >= SERIAL_TIMER_INTERVAL_MS) {
    lastSerialTimerReport = millis();
    printBackgroundTimerToSerial();
  }

  // --- HARDENED COIN PULSE PROCESSING ---
  uint16_t currentPulses = 0;
  unsigned long lastTimeUs = 0;

  noInterrupts();
  currentPulses = isrPulseCount;
  lastTimeUs = lastPulseTimeUs;
  interrupts();

  if (newPulseDetected) {
    digitalWrite(LED_PIN, HIGH);
    newPulseDetected = false;
  }

  if (!INSTANT_MODE && currentPulses > 0 && (micros() - lastTimeUs >= (COIN_TIMEOUT_MS * 1000UL))) {
    if (digitalRead(COIN_PIN) == HIGH) {
      noInterrupts();
      accumulatedPulses = isrPulseCount;
      isrPulseCount = 0; 
      interrupts();

      if (accumulatedPulses > 0) {
        processDetectedCoin(accumulatedPulses);
      }
    } else {
      noInterrupts();
      isrPulseCount = 0;
      interrupts();
    }
    digitalWrite(LED_PIN, LOW);
  }

  // --- KEY CHORD: LOCKER + RED BUTTON (ADMIN LOCKER OVERRIDE) ---
  if (btnRed.isPressedRaw()) {
    if (btn1.isPressedRaw()) {
      if (adminModeActive) {
        triggerBuzzer(BUZZ_ERROR);
        triggerTemporaryMessage("ADMIN MODE", "CANCELLED / EXIT", 1200);
      } else {
        adminModeActive = true;
        adminTargetLocker = 1;
        triggerBuzzer(BUZZ_SELECT);
        processAdminScanAndOverride();
      }
      return;
    } 
    else if (btn2.isPressedRaw()) {
      if (adminModeActive) {
        triggerBuzzer(BUZZ_ERROR);
        triggerTemporaryMessage("ADMIN MODE", "CANCELLED / EXIT", 1200);
      } else {
        adminModeActive = true;
        adminTargetLocker = 2;
        triggerBuzzer(BUZZ_SELECT);
        processAdminScanAndOverride();
      }
      return;
    }
  }

  // --- KEY CHORD: LOCKER + GREEN BUTTON (ADMIN TIMER EMERGENCY RESET) ---
  if (btnGreen.isPressedRaw()) {
    if (btn1.isPressedRaw()) {
      if (adminModeActive) {
        triggerBuzzer(BUZZ_ERROR);
        triggerTemporaryMessage("TIMER RESET", "CANCELLED / EXIT", 1200);
      } else {
        adminModeActive = true;
        adminTargetLocker = 1;
        triggerBuzzer(BUZZ_SELECT);
        processAdminTimerReset();
      }
      return;
    }
    else if (btn2.isPressedRaw()) {
      if (adminModeActive) {
        triggerBuzzer(BUZZ_ERROR);
        triggerTemporaryMessage("TIMER RESET", "CANCELLED / EXIT", 1200);
      } else {
        adminModeActive = true;
        adminTargetLocker = 2;
        triggerBuzzer(BUZZ_SELECT);
        processAdminTimerReset();
      }
      return;
    }
  }

  // --- STANDARD SINGLE BUTTON HANDLING LOGIC ---
  if (btn1.wasPressed()) {
    if (pendingAction != NONE) {
      executeAction(1, pendingAction);
    } else {
      selectedWindow = 1;
      showIdleScreen();
    }
  }

  if (btn2.wasPressed()) {
    if (pendingAction != NONE) {
      executeAction(2, pendingAction);
    } else {
      selectedWindow = 2;
      showIdleScreen();
    }
  }

  if (btnBlue.wasPressed()) {
    if (selectedWindow != 0) {
      executeAction(selectedWindow, ACTION_OPEN);
    } else {
      pendingAction = ACTION_OPEN;
      showIdleScreen();
    }
  }

  if (btnRed.wasPressed()) {
    handleRedButton();
  }

  if (btnGreen.wasPressed()) {
    handleGreenButton();
  }
}