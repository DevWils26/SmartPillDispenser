#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <RTClib.h>
#include <EEPROM.h>
#include <avr/sleep.h>
#include <avr/power.h>

#define BTN_UP 22
#define BTN_SELECT 23
#define BTN_DOWN 24
#define BTN_BACK 25
#define SS_PIN 53
#define RST_PIN 49
#define NUM_PILLS 3
#define SCHED_SLOTS_PER_DAY 4
#define SCHED_DAYS 7
#define BUZZER_PIN 32
#define WAKE_BTN_PIN 2
#define SLEEP_AFTER_MS 60000UL

bool postDispenseActive = false;
unsigned long postDispenseStartMs = 0;
const unsigned long POST_DISPENSE_WAIT_MS = 30000UL;
unsigned long lastBeepMs = 0;

const int motorPins[3][4] = {
  {36, 37, 38, 39}, 
  {40, 41, 42, 43},   
  {44, 45, 46, 47}   
};

int delayTime = 4;

const int seq[4][4] = {
  {1,1,0,0},
  {0,1,1,0},
  {0,0,1,1},
  {1,0,0,1}
};

int motorPhase[3] = {0, 0, 0};

const long STEPS_PER_REV = 2048;
const long QUARTER_TURN = STEPS_PER_REV / 4;

const String MASTER_UID = "313A19AA";
const String USER_UID   = "0EFE2B1F";

volatile bool wokeFlag = false;
unsigned long lastActivityMs = 0;

struct PendingDose {
  bool active;
  uint8_t pill; 
  uint8_t slot;  
  uint8_t dayIdx; 
  uint8_t hour;
  uint8_t minute;
  unsigned long startMs;
};

PendingDose pending = {false, 0, 0, 0, 0, 0, 0};

uint8_t edit_hours = 1;
uint8_t edit_minutes = 1;
uint8_t edit_seconds = 1;
uint8_t edit_month = 1;
uint8_t edit_day = 1;
uint16_t edit_year = 2026;

const uint8_t EMPTY = 0xFF;
const char* dayNames[SCHED_DAYS] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
const int SCHED_BYTES = NUM_PILLS * SCHED_DAYS * SCHED_SLOTS_PER_DAY * 2;
const int SCHED_BASE = 0;

LiquidCrystal_I2C lcd(0x27, 20, 4);
MFRC522 rfid(SS_PIN, RST_PIN);
RTC_DS3231 rtc;
const char* menuItems[] = {"Set Time/Schedule", "Check Medication"};
const int menuLength = sizeof(menuItems) / sizeof(menuItems[0]);

struct MissedLog {
  uint8_t used;      
  uint8_t pill;     
  uint8_t slot;     
  uint8_t month;
  uint8_t day;
  uint16_t year;
  uint8_t hour;
  uint8_t minute;
};

const int LOG_BASE = SCHED_BASE + SCHED_BYTES;
const int MAX_LOGS = 20;

int currentSelection = 0;
bool inMenu = false;
int button_in_menu = 0;
int med1;
int med2;
int med3;

const int MED1_ADDR = LOG_BASE + MAX_LOGS * sizeof(MissedLog);
const int MED2_ADDR = MED1_ADDR + sizeof(int);
const int MED3_ADDR = MED2_ADDR + sizeof(int);

int8_t lastDispensedMinute[NUM_PILLS][2];

int logAddr(int index){
  return LOG_BASE + index * sizeof(MissedLog);
}

void saveMedCounts(){
  EEPROM.put(MED1_ADDR, med1);
  EEPROM.put(MED2_ADDR, med2);
  EEPROM.put(MED3_ADDR, med3);
}

void loadMedCounts(){
  EEPROM.get(MED1_ADDR, med1);
  EEPROM.get(MED2_ADDR, med2);
  EEPROM.get(MED3_ADDR, med3);

  if (med1 < 0 || med1 > 500){
    med1 = 0;
  }
  if (med2 < 0 || med2 > 500){ 
    med2 = 0;
  }
  if (med3 < 0 || med3 > 500){ 
    med3 = 0;
  }
}

void clearAllLogs(){
  MissedLog blank;
  blank.used = 0;
  blank.pill = 0;
  blank.slot = 0;
  blank.month = 0;
  blank.day = 0;
  blank.year = 0;
  blank.hour = 0;
  blank.minute = 0;

  for (int i = 0; i < MAX_LOGS; i++){
    EEPROM.put(logAddr(i), blank);
  }
}

int countLogs(){
  int count = 0;
  MissedLog log;

  for (int i = 0; i < MAX_LOGS; i++){
    EEPROM.get(logAddr(i), log);
    if (log.used == 1){
      count++;
    }
  }
  return count;
}

void addMissedLog(uint8_t pill, uint8_t slot, DateTime when){
  MissedLog log;
  log.used = 1;
  log.pill = pill;
  log.slot = slot;
  log.month = when.month();
  log.day = when.day();
  log.year = when.year();
  log.hour = when.hour();
  log.minute = when.minute();

  for (int i = 0; i < MAX_LOGS; i++){
    MissedLog temp;
    EEPROM.get(logAddr(i), temp);

    if (temp.used != 1){
      EEPROM.put(logAddr(i), log);
      return;
    }
  }

  EEPROM.put(logAddr(MAX_LOGS - 1), log);
}

void startBeep() {
  for(int i = 0; i < 3; i++) {
    tone(BUZZER_PIN, 800);
    delay(200);

    tone(BUZZER_PIN, 1000);
    delay(200);
  }

  noTone(BUZZER_PIN);
}

void stopBeep() {
  noTone(BUZZER_PIN);
}

void initDispenseFlags(){
  for (int p = 0; p < NUM_PILLS; p++){
    for (int t = 0; t < 2; t++){
      lastDispensedMinute[p][t] = -1;
    }
  }
}

void wakeISR(){
  wokeFlag = true;
}

void markActivity(){
  lastActivityMs = millis();
}

void goToSleep(){
  if (pending.active){
    return;
  }

  lcd.noBacklight();

  wokeFlag = false;

  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_enable();

  #if defined(BODS) && defined(BODSE)
    sleep_bod_disable();
  #endif

  noInterrupts();
  if (!wokeFlag){
    interrupts();
    sleep_cpu();
  } 
  else {
    interrupts();
  }

  sleep_disable();

  lcd.backlight();
  markActivity();
}

void setMotorCoils(int motor, int a, int b, int c, int d){
  digitalWrite(motorPins[motor][0], a);
  digitalWrite(motorPins[motor][1], b);
  digitalWrite(motorPins[motor][2], c);
  digitalWrite(motorPins[motor][3], d);
}

void motorOff(int motor){
  setMotorCoils(motor, 0, 0, 0, 0);
}

void stepMotorOnce(int motor, int dir){
  motorPhase[motor] += dir;

  if (motorPhase[motor] > 3){ 
    motorPhase[motor] = 0;
  }
  if (motorPhase[motor] < 0){
    motorPhase[motor] = 3;
  }

  setMotorCoils(
    motor,
    seq[motorPhase[motor]][0],
    seq[motorPhase[motor]][1],
    seq[motorPhase[motor]][2],
    seq[motorPhase[motor]][3]
  );

  delay(delayTime);
}

void moveMotorSteps(int motor, long steps, int dir){
  for (long i = 0; i < steps; i++){
    stepMotorOnce(motor, dir);
  }
  motorOff(motor);
}

void quarterTurnCW(int motor){
  moveMotorSteps(motor, QUARTER_TURN, +1);
}

void quarterTurnCCW(int motor){
  moveMotorSteps(motor, QUARTER_TURN, -1);
}

bool checkRFIDOnce(String &uidOut){
  if (!rfid.PICC_IsNewCardPresent()) {
    Serial.println("No new card present");
    return false;
  }

  Serial.println("Card detected");

  if (!rfid.PICC_ReadCardSerial()) {
    Serial.println("Could not read card serial");
    return false;
  }

  uidOut = "";
  for (byte i = 0; i < rfid.uid.size; i++){
    if (rfid.uid.uidByte[i] < 0x10){ 
      uidOut += "0";
    }
    uidOut += String(rfid.uid.uidByte[i], HEX);
  }
  uidOut.toUpperCase();

  Serial.print("UID read: ");
  Serial.println(uidOut);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  return true;
}

bool isAuthorizedUID(const String &uid){
  return (uid == USER_UID);
}

bool isMasterUID(const String &uid){
  return (uid == MASTER_UID);
}

int masterPillSelectMenu(){
  int sel = 0;

  while (true){
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Master Dispense");

    lcd.setCursor(0,1);
    lcd.print(sel == 0 ? ">" : " ");
    lcd.print("Pill 1");

    lcd.setCursor(0,2);
    lcd.print(sel == 1 ? ">" : " ");
    lcd.print("Pill 2");

    lcd.setCursor(10,1);
    lcd.print(sel == 2 ? ">" : " ");
    lcd.print("Pill 3");

    lcd.setCursor(0,3);
    lcd.print("Up/Dn Sel=OK Back");

    if (pressed(BTN_UP)){
      sel--;
      if (sel < 0) sel = 2;
    }

    if (pressed(BTN_DOWN)){
      sel++;
      if (sel > 2) sel = 0;
    }

    if (pressed(BTN_SELECT)){
      return sel;
    }

    if (pressed(BTN_BACK)){
      return -1;
    }
  }
}

void masterMenu(){
  int sel = 0;
  const int itemCount = 3;
  const char* items[itemCount] = {
    "Manual Dispense",
    "Check Logs",
    "Clear Logs"
  };

  while (true){
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(sel == 0 ? ">" : " ");
    lcd.print(items[0]);

    lcd.setCursor(0,1);
    lcd.print(sel == 1 ? ">" : " ");
    lcd.print(items[1]);

    lcd.setCursor(0,2);
    lcd.print(sel == 2 ? ">" : " ");
    lcd.print(items[2]);

    lcd.setCursor(0,3);
    lcd.print("Up/Dn Sel Back");

    if (pressed(BTN_UP)){
      sel--;
      if (sel < 0){ 
        sel = itemCount - 1;
      }
    }

    if (pressed(BTN_DOWN)){
      sel++;
      if (sel >= itemCount){
        sel = 0;
      }
    }

    if (pressed(BTN_SELECT)){
      if (sel == 0){
        int selectedPill = masterPillSelectMenu();

        if (selectedPill != -1){
          lcd.clear();
          lcd.setCursor(0,0);
          lcd.print("Dispensing Pill ");
          lcd.print(selectedPill + 1);
          delay(700);
          dispensePill(selectedPill, -1);
        }
      }
      else if (sel == 1){
        check_log();
      }
      else if (sel == 2){
        clearLogsMenu();
      }
    }

    if (pressed(BTN_BACK)){
      lcd.clear();
      return;
    }
  }
}

void handleMasterRFID(){
  String uid;

  if (!checkRFIDOnce(uid)){
    return;
  }

  if (!isMasterUID(uid)){
    return;
  }

  stopBeep();

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Master Card OK");
  delay(700);

  masterMenu();
}

void processPendingDose(){
  if (!pending.active){
    return;
  }

  const unsigned long TIMEOUT_MS = 30000;
  const unsigned long BEEP_INTERVAL_MS = 5000;

  unsigned long elapsed = millis() - pending.startMs;

  if (millis() - lastBeepMs >= BEEP_INTERVAL_MS){
    tone(BUZZER_PIN, 1000, 300);
    lastBeepMs = millis();
  }

  unsigned long remainingMs = 0;
  if (elapsed < TIMEOUT_MS){
    remainingMs = TIMEOUT_MS - elapsed;
  }

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Take your pill");
  lcd.setCursor(0,1);
  lcd.print("Scan fob to ok");
  lcd.setCursor(0,2);
  lcd.print("Time left: ");
  lcd.print(remainingMs / 1000);
  lcd.print(" sec   ");

  if (elapsed >= TIMEOUT_MS){
    noTone(BUZZER_PIN);

    DateTime now = rtc.now();
    addMissedLog(pending.pill, pending.slot, now);

    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Dose missed");
    lcd.setCursor(0,1);
    lcd.print("Logged in system");
    delay(1500);

    pending.active = false;
    return;
  }

  String uid;
  if (checkRFIDOnce(uid)){
    if (isAuthorizedUID(uid)){
      noTone(BUZZER_PIN);

      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("RFID accepted");
      delay(500);

      dispensePill(pending.pill, pending.slot);

      pending.active = false;
    }
    else if (isMasterUID(uid)){
      noTone(BUZZER_PIN);
      pending.active = false;
      masterMenu();
    }
  }
}

int rtcDayToIndex(uint8_t rtcDay){
  if (rtcDay == 0){
    return 6;
  }
  return rtcDay - 1;         
}

void checkAndDispense(){
  DateTime now = rtc.now();

  int dayIdx = rtcDayToIndex(now.dayOfTheWeek());
  uint8_t curHour = now.hour();
  uint8_t curMin  = now.minute();

  for (int pill = 0; pill < NUM_PILLS; pill++){
    for (int slot = 0; slot < 2; slot++){

      uint8_t hour = EEPROM.read(addrFor(pill, dayIdx, slot, 0));
      uint8_t min  = EEPROM.read(addrFor(pill, dayIdx, slot, 1));

      if (hour == EMPTY || min == EMPTY){
        continue;
      }

      if (hour == curHour && min == curMin){

        if (lastDispensedMinute[pill][slot] == curMin){
          continue;
        }

        if (pending.active){
          continue;
        }

        lastDispensedMinute[pill][slot] = curMin;

        pending.active = true;
        pending.pill = pill;
        pending.slot = slot;
        pending.dayIdx = dayIdx;
        pending.hour = hour;
        pending.minute = min;
        pending.startMs = millis();
        lastBeepMs = 0;

        lcd.clear();
        lcd.setCursor(0,0);
        lcd.print("Pill due now");
        lcd.setCursor(0,1);
        lcd.print("Scan fob to");
        lcd.setCursor(0,2);
        lcd.print("dispense");
        
        return;
      }
    }
  }
}

void dispensePill(int pill, int slot){
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Dispensing Pill ");
  lcd.print(pill + 1);

  lcd.setCursor(0,1);
  if (slot == 0){
    lcd.print("Morning dose");
  }
  else if (slot == 1){
    lcd.print("Night dose");
  }
  else{
    lcd.print("Manual dispense");
  }

  if (pill >= 0 && pill < 3){
    quarterTurnCW(pill);
    quarterTurnCW(pill);

    if (pill == 0 && med1 > 0){
      med1--;
    }
    else if (pill == 1 && med2 > 0){
      med2--;
    }
    else if (pill == 2 && med3 > 0){
      med3--;
    }
    saveMedCounts();
  }
  startBeep();
  delay(1000);
}

void processPostDispenseWait(){
  if (!postDispenseActive){
    return;
  }

  unsigned long elapsed = millis() - postDispenseStartMs;
  unsigned long remaining = 0;

  if (elapsed < POST_DISPENSE_WAIT_MS){
    remaining = (POST_DISPENSE_WAIT_MS - elapsed) / 1000;
  }

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Pill dispensed");
  lcd.setCursor(0,1);
  lcd.print("Please take pill");
  lcd.setCursor(0,2);
  lcd.print("Time left: ");
  lcd.print(remaining);
  lcd.print(" sec   ");

  if (elapsed >= POST_DISPENSE_WAIT_MS){
    postDispenseActive = false;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Pickup time over");
    delay(1000);
  }
}

void time_set(){

  DateTime now = rtc.now();
  edit_hours   = now.hour();
  edit_minutes = now.minute();
  edit_seconds = now.second();

  int field = 0;

  while (true){

    lcd.clear();

    lcd.setCursor(0,0);
    if(field == 0){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Hours:   ");
    lcd.print(edit_hours);

    lcd.setCursor(0,1);
    if(field == 1){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Minutes: ");
    lcd.print(edit_minutes);

    lcd.setCursor(0,2);
    if(field == 2){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Seconds: ");
    lcd.print(edit_seconds);

    lcd.setCursor(0,3);
    lcd.print("S=Next B=Save");

    delay(150);

    if (digitalRead(BTN_SELECT) == LOW){
      field++;
      if(field > 2){
        field = 0;
      }
      delay(200);
    }

    if (digitalRead(BTN_UP) == LOW){
      if(field == 0){
        edit_hours = (edit_hours + 1) % 24;
      }
      else if(field == 1){
        edit_minutes = (edit_minutes + 1) % 60;
      }
      else if(field == 2){
        edit_seconds = (edit_seconds + 1) % 60;
      }
      delay(200);
    }

    if (digitalRead(BTN_DOWN) == LOW){
      if(field == 0){
        edit_hours = (edit_hours == 0) ? 23 : edit_hours - 1;
      }
      else if(field == 1){
        edit_minutes = (edit_minutes == 0) ? 59 : edit_minutes - 1;
      }
      else if(field == 2){
        edit_seconds = (edit_seconds == 0) ? 59 : edit_seconds - 1;
      }
      delay(200);
    }

    if (digitalRead(BTN_BACK) == LOW){

      DateTime current = rtc.now();

      rtc.adjust(DateTime(
        edit_year,
        edit_month,
        edit_day,
        edit_hours,
        edit_minutes,
        edit_seconds
      ));

      lcd.clear();
      return;
    }
  }
}

void date_set(){
  DateTime now = rtc.now();
  edit_day = now.day();
  edit_month = now.month();
  edit_year = now.year();

  int field = 0;

  while (true){

    lcd.clear();

    lcd.setCursor(0,0);
    if(field == 0){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Day:   ");
    lcd.print(edit_day);

    lcd.setCursor(0,1);
    if(field == 1){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Month: ");
    lcd.print(edit_month);

    lcd.setCursor(0,2);
    if(field == 2){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Year: ");
    lcd.print(edit_year);

    lcd.setCursor(0,3);
    lcd.print("S=Next B=Save");

    delay(150);

    if (digitalRead(BTN_SELECT) == LOW){
      field++;
      if(field > 2){
        field = 0;
      }
      delay(200);
    }

    if (digitalRead(BTN_UP) == LOW){
      if(field == 0){
          edit_day++;
        if(edit_day > 31){ 
          edit_day = 1;
        }
      }
      else if(field == 1){
        edit_month++;
        if(edit_month > 12){
          edit_month = 1;
        }
      }
      if(field == 2){
        edit_year++;
      }
      delay(200);
    }

    if (digitalRead(BTN_DOWN) == LOW){
      if(field == 0){
        edit_day--;
        if(edit_day < 1){
          edit_day = 31;
        }
      }
      else if(field == 1){
        edit_month--;
        if(edit_month < 1){
          edit_month = 12;
        }
      }
      if(field == 2){
        edit_year--;
      }
      delay(200);
    }

    if (digitalRead(BTN_BACK) == LOW){

      DateTime current = rtc.now();

      rtc.adjust(DateTime(
        edit_year,
        edit_month,
        edit_day,
        edit_hours,
        edit_minutes,
        edit_seconds
      ));

      lcd.clear();
      return;
    }
  }
}

void RTC_SetMenu(void){
  currentSelection = 0;
  char* RTC_menu[] = {"Time","Date"};
  while(true) {
    for (int i = 0; i < 2; i++){
      lcd.setCursor(0, i);
      if (i == currentSelection){
        lcd.print(">");
      } 
      else {
        lcd.print(" ");
      }
      lcd.print(RTC_menu[i]);
    }
    lcd.setCursor(0, 3);
    lcd.print("Select          Back");

    if (digitalRead(BTN_UP) == LOW){
      delay(100);
      currentSelection -= 1;
      if (currentSelection <= -1){
        currentSelection = 1;
      }
    }
    if (digitalRead(BTN_DOWN) == LOW){
      delay(100);
      currentSelection += 1;
      if (currentSelection >= 2){
        currentSelection = 0;
      }
    }
    if (digitalRead(BTN_SELECT) == LOW){
      delay(100);
      if (currentSelection == 0){
        lcd.clear();
        time_set();
      }
      else if (currentSelection == 1){
        lcd.clear();
        date_set();
      }
    }
    if (digitalRead(BTN_BACK) == LOW){
      delay(100);
      lcd.clear();
      return;
    }
  }
  delay(1000);
}

static void waitRelease(int pin){
  while (digitalRead(pin) == LOW){ 
    delay(10); 
  }
  delay(80);
}

static bool pressed(int pin){
  if (digitalRead(pin) == LOW){
    waitRelease(pin);
    lastActivityMs = millis();
    return true;
  }
  return false;
}

static void print2(uint8_t v){
  if (v < 10){
    lcd.print("0");
  }
  lcd.print(v);
}

static int addrFor(int pill, int day, int timeIdx, int field){
  int slot = (((pill * SCHED_DAYS) + day) * SCHED_SLOTS_PER_DAY + timeIdx);
  return SCHED_BASE + slot * 2 + field;
}

void clearScheduleEEPROM(){
  for (int i = 0; i < SCHED_BYTES; i++){
    EEPROM.update(SCHED_BASE + i, EMPTY);
  }
}

int8_t promptYesNo(const char* title){
  bool yes = true;

  while (true){
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(title);

    lcd.setCursor(0,2);
    if (yes) {
      lcd.print(">Yes");
    } 
    else {
    lcd.print(" Yes");
    }
    lcd.setCursor(10,2);
    if (!yes) {
      lcd.print(">No");
    } 
    else {
      lcd.print(" No");
    }

    lcd.setCursor(0,3);
    lcd.print("Sel=OK  Up/Down");

    if (pressed(BTN_UP) || pressed(BTN_DOWN)){
      yes = !yes;
    }

    if (pressed(BTN_SELECT)){
      if (yes) {
        return 1;
      } 
      else {
        return 0;
      }
    }

    if (pressed(BTN_BACK)){
      return -1;
    }
  }
}

bool editNumber(const char* label, uint8_t& value, uint8_t minV, uint8_t maxV){
  while (true){
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print(label);
    lcd.setCursor(0,1);
    lcd.print("Value: ");
    lcd.print(value);

    lcd.setCursor(0,3);
    lcd.print("Up/Down  Sel=OK");

    if (pressed(BTN_UP)){
      if (value >= maxV) value = minV;
      else {
        value++;
      }
    }
    if (pressed(BTN_DOWN)){
      if (value <= minV) value = maxV;
      else {
        value--;
      }
    }
    if (pressed(BTN_SELECT)){ 
      return true;
    }
    if (pressed(BTN_BACK)){
      return false;
    }
  }
}

void setDayTimes(int pill, int day){
  for (int t = 0; t < SCHED_SLOTS_PER_DAY; t++){
    EEPROM.update(addrFor(pill, day, t, 0), EMPTY);
    EEPROM.update(addrFor(pill, day, t, 1), EMPTY);
  }

  {
    char prompt[21];
    snprintf(prompt, sizeof(prompt), "%s P%d Morning?", dayNames[day], pill + 1);

    int8_t ans = promptYesNo(prompt);
    if (ans == -1){
      return;
    }    
    if (ans == 1){
      uint8_t hour = 8;
      uint8_t minute = 0;

      char labelH[21];
      snprintf(labelH, sizeof(labelH), "%s P%d M Hr", dayNames[day], pill + 1);
      if (!editNumber(labelH, hour, 0, 23)){
        return;
      }

      char labelM[21];
      snprintf(labelM, sizeof(labelM), "%s P%d M Min", dayNames[day], pill + 1);
      if (!editNumber(labelM, minute, 0, 59)){
        return;
      }

      EEPROM.update(addrFor(pill, day, 0, 0), hour);
      EEPROM.update(addrFor(pill, day, 0, 1), minute);

      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Saved Morning ");
      lcd.setCursor(0,1);
      print2(hour); lcd.print(":"); print2(minute);
      delay(700);
    }
 
  }

  {
    char prompt[21];
    snprintf(prompt, sizeof(prompt), "%s P%d Night?", dayNames[day], pill + 1);

    int8_t ans = promptYesNo(prompt);
    if (ans == -1){
      return;
    }
    if (ans == 1){
      uint8_t hour = 20;
      uint8_t minute = 0;

      char labelH[21];
      snprintf(labelH, sizeof(labelH), "%s P%d N Hr", dayNames[day], pill + 1);
      if (!editNumber(labelH, hour, 0, 23)){
        return;
      }

      char labelM[21];
      snprintf(labelM, sizeof(labelM), "%s P%d N Min", dayNames[day], pill + 1);
      if (!editNumber(labelM, minute, 0, 59)){
        return;
      }

      EEPROM.update(addrFor(pill, day, 1, 0), hour);
      EEPROM.update(addrFor(pill, day, 1, 1), minute);

      lcd.clear();
      lcd.setCursor(0,0);
      lcd.print("Saved Night ");
      lcd.setCursor(0,1);
      print2(hour); lcd.print(":"); print2(minute);
      delay(700);
    }
    
  }

}

void setPillSchedule(int pill){
  int sel = 0;

  while (true){
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Set Pill ");
    lcd.print(pill + 1);

    int top = sel;
    if (top > SCHED_DAYS - 3){
      top = SCHED_DAYS - 3;
    }
    if (top < 0){
      top = 0;
    }

    for (int row = 0; row < 3; row++){
      int d = top + row;
      lcd.setCursor(0, row + 1);
      lcd.print(d == sel ? ">" : " ");
      lcd.print(dayNames[d]);
      lcd.print("  (Sel)");
    }

    if (pressed(BTN_UP)){
      sel = (sel - 1 + SCHED_DAYS) % SCHED_DAYS;
    }
    if (pressed(BTN_DOWN)){
      sel = (sel + 1) % SCHED_DAYS;
    }
    if (pressed(BTN_SELECT)){
      setDayTimes(pill, sel);
    }
    if (pressed(BTN_BACK)){
      return;
    }
  }
}

void createNewScheduleWizard(){
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Clearing schedule");
  clearScheduleEEPROM();
  delay(700);

  for (int pill = 0; pill < NUM_PILLS; pill++){
    char title[21];
    snprintf(title, sizeof(title), "Set Pill %d sched?", pill + 1);

    int8_t ans = promptYesNo(title);
    if (ans == -1){
      break;  
    }
    if (ans == 0){
      continue;
    }

    setPillSchedule(pill);
  }

  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Schedule setup done");
  delay(900);
}

void existing_sched(){

  lcd.clear();
  lcd.print("implement later");
  delay(1000);

}

void Sched_SetMenu(){
  currentSelection = 0;
  char* sched_menuItems[] = {"Create New", "Add to Existing"};
  int sched_menuLength = sizeof(sched_menuItems) / sizeof(sched_menuItems[0]);
  while(true) {
    for (int i = 0; i < sched_menuLength; i++){
      lcd.setCursor(0, i);
      if (i == currentSelection){
        lcd.print(">");
      } 
      else {
        lcd.print(" ");
      }
      lcd.print(sched_menuItems[i]);
    }
    lcd.setCursor(0, 3);
    lcd.print("Select          Back");

    if (digitalRead(BTN_UP) == LOW){
      delay(100);
      currentSelection += 1;
      if (currentSelection >= 2){
        currentSelection = 0;
      }
    }
    if (digitalRead(BTN_DOWN) == LOW){
      delay(100);
      currentSelection -= 1;
      if (currentSelection <= -1){
        currentSelection = 1;
      }
    }
    if (digitalRead(BTN_SELECT) == LOW){
      delay(100);
      if (currentSelection == 0){
        lcd.clear();
        createNewScheduleWizard();
      }
      else if (currentSelection == 1){
        lcd.clear();
        existing_sched();
      }
    }
    if (digitalRead(BTN_BACK) == LOW){
      delay(100);
      lcd.clear();
      return;
    }
  }
  delay(1000);

}

void set_time_sched(){
  currentSelection = 0;
  char* sched_menuItems[] = {"Set Time/Date", "Set Schedule"};
  int sched_menuLength = sizeof(sched_menuItems) / sizeof(sched_menuItems[0]);
  while(true) {
    for (int i = 0; i < sched_menuLength; i++){
      lcd.setCursor(0, i);
      if (i == currentSelection){
        lcd.print(">");
      } 
      else {
        lcd.print(" ");
      }
      lcd.print(sched_menuItems[i]);
    }
    lcd.setCursor(0, 3);
    lcd.print("Select          Back");

    if (digitalRead(BTN_UP) == LOW){
      delay(100);
      currentSelection += 1;
      if (currentSelection >= 2){
        currentSelection = 0;
      }
    }
    if (digitalRead(BTN_DOWN) == LOW){
      delay(100);
      currentSelection -= 1;
      if (currentSelection <= -1){
        currentSelection = 1;
      }
    }
    if (digitalRead(BTN_SELECT) == LOW){
      delay(100);
      if (currentSelection == 0){
        lcd.clear();
        RTC_SetMenu();
      }
      else if (currentSelection == 1){
        lcd.clear();
        Sched_SetMenu();
      }
    }
    if (digitalRead(BTN_BACK) == LOW){
      delay(100);
      lcd.clear();
      return;
    }
  }
  delay(1000);
}

void check_med(){
  int field = 0;

  while (true) {

    lcd.clear();

    lcd.setCursor(0,0);
    if(field == 0){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Pill 1: ");
    lcd.print(med1);

    lcd.setCursor(0,1);
    if(field == 1){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Pill 2: ");
    lcd.print(med2);

    lcd.setCursor(0,2);
    if(field == 2){
      lcd.print(">");
    }
    else{
      lcd.print(" ");
    }
    lcd.print("Pill 3: ");
    lcd.print(med3);

    lcd.setCursor(0,3);
    lcd.print("S=Next B=Save");

    delay(150);

    if (digitalRead(BTN_SELECT) == LOW){
      field++;
      if(field > 2) field = 0;
      delay(200);
    }

    if (digitalRead(BTN_UP) == LOW){
      if(field == 0){
        med1++;
      }
      else if(field == 1){
        med2++;
      }
      if(field == 2){
        med3++;
      }
      delay(200);
    }

    if (digitalRead(BTN_DOWN) == LOW){
      if(field == 0){
        med1--;
        if(med1 < 0){
          med1 = 0;
        }
      }
      else if(field == 1){
        med2--;
        if(med2 < 0){
          med2 = 0;
        }
      }
      if(field == 2){
        med3--;
        if(med3 < 0){
          med3 = 0;
        }
      }
      delay(200);
    }

    if (digitalRead(BTN_BACK) == LOW){
      saveMedCounts();
      delay(100);
      lcd.clear();
      return;
    }
  }
}

void clearLogsMenu(){
  int8_t ans = promptYesNo("Clear all logs?");

  if (ans == 1){
    clearAllLogs();
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("Logs cleared");
    delay(1000);
  }
}

void check_log(){
  int index = 0;

  while (true){
    int total = countLogs();

    lcd.clear();

    if (total == 0){
      lcd.setCursor(0,0);
      lcd.print("No missed logs");
      lcd.setCursor(0,3);
      lcd.print("Back to exit");

      if (pressed(BTN_BACK)){
        return;
      }
      continue;
    }

    if (index < 0){
      index = 0;
    }
    if (index >= total){
      index = total - 1;
    }

    MissedLog current;
    int found = -1;
    int seen = 0;

    for (int i = 0; i < MAX_LOGS; i++){
      EEPROM.get(logAddr(i), current);
      if (current.used == 1){
        if (seen == index){
          found = i;
          break;
        }
        seen++;
      }
    }

    if (found == -1){
      return;
    }

    EEPROM.get(logAddr(found), current);

    lcd.setCursor(0,0);
    lcd.print("Log ");
    lcd.print(index + 1);
    lcd.print("/");
    lcd.print(total);

    lcd.setCursor(0,1);
    lcd.print("P");
    lcd.print(current.pill + 1);
    lcd.print(" ");
    lcd.print(current.slot == 0 ? "Morning" : "Night");

    lcd.setCursor(0,2);
    lcd.print(current.month);
    lcd.print("/");
    lcd.print(current.day);
    lcd.print("/");
    lcd.print(current.year);

    lcd.setCursor(0,3);
    if (current.hour < 10){
      lcd.print("0");
    }
    lcd.print(current.hour);
    lcd.print(":");
    if (current.minute < 10){
      lcd.print("0");
    }
    lcd.print(current.minute);
    lcd.print(" U/D B=Exit");

    if (pressed(BTN_UP)){
      index--;
      if (index < 0){
        index = total - 1;
      }
    }

    if (pressed(BTN_DOWN)){
      index++;
      if (index >= total){
        index = 0;
      }
    }

    if (pressed(BTN_BACK)){
      return;
    }
  }
}

void showMenu(){
  currentSelection = 0;
  lcd.clear();
  while(button_in_menu == 1){
    for (int i = 0; i < menuLength; i++){
      lcd.setCursor(0, i);
      if (i == currentSelection){
        lcd.print(">");
      } 
      else {
        lcd.print(" ");
      }
      lcd.print(menuItems[i]);
    }
    lcd.setCursor(0, 3);
    lcd.print("Select          Back");

    if (digitalRead(BTN_UP) == LOW){
      delay(100);
      currentSelection -= 1;
      if (currentSelection <= -1){
        currentSelection = menuLength - 1;
      }
    }
    if (digitalRead(BTN_DOWN) == LOW){
      delay(100);
      currentSelection += 1;
      if (currentSelection >= menuLength){
        currentSelection = 0;
      }
    }
    if (digitalRead(BTN_SELECT) == LOW){
      delay(100);
      if (currentSelection == 0){
        lcd.clear();
        set_time_sched();
      }
      else if (currentSelection == 1){
        lcd.clear();
        check_med();
      }
    }
    if (digitalRead(BTN_BACK) == LOW){
      delay(100);
      button_in_menu = 0;
      inMenu = false;
      lcd.clear();
      return;
    }
  }
}

void setup(){
  Serial.begin(9600);
  SPI.begin();
  rfid.PCD_Init();
  initDispenseFlags();

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  pinMode(WAKE_BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(WAKE_BTN_PIN), wakeISR, FALLING);

  lastActivityMs = millis();

  for (int m = 0; m < 3; m++){
    for (int p = 0; p < 4; p++){
      pinMode(motorPins[m][p], OUTPUT);
    } 
    motorOff(m);
  }

  lcd.init();
  lcd.backlight();

  if (!rtc.begin()) {
    lcd.setCursor(0,1);
    lcd.print("RTC not found!");
    while(1);
  }

  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  loadMedCounts();
}

void loop(){
  checkAndDispense();
  processPendingDose();

  if (pending.active){
    return;
  }

  handleMasterRFID();

  DateTime now = rtc.now();
  edit_hours = now.hour();
  edit_minutes = now.minute();
  edit_seconds = now.second();
  edit_month = now.month();
  edit_day = now.day();
  edit_year = now.year();

  lcd.setCursor(0,0);
  lcd.print("                    ");
  lcd.setCursor(0,0);
  lcd.print("      ");
  lcd.print(now.month());
  lcd.print("/");
  lcd.print(now.day());
  lcd.print("/");
  lcd.print(now.year());

  lcd.setCursor(0,1);
  lcd.print("                    ");
  lcd.setCursor(0,1);
  lcd.print("      ");
  if(now.hour()<10){
    lcd.print("0");
  }
  lcd.print(now.hour());
  lcd.print(":");
  if(now.minute()<10){
    lcd.print("0");
  }
  lcd.print(now.minute());
  lcd.print(":");
  if(now.second()<10){
    lcd.print("0");
  }
  lcd.print(now.second());

  int raw = analogRead(A0);
  float voltage = raw * (5.0 / 1023.0);
  float battery = voltage * 2;
  float percent_battery = ((battery - 4.0) / (7.3 - 4.0)) * 100.0;
  if (percent_battery < 0){
    percent_battery = 0;
  }
  if (percent_battery > 100){
    percent_battery = 100;
  }

  lcd.setCursor(0, 2);
  lcd.print("     Battery:");
  lcd.print(percent_battery,0);
  lcd.print("%   ");

  lcd.setCursor(0,3);
  lcd.print(" Click SEL for Menu");

  if (!inMenu){
    if (digitalRead(BTN_SELECT) == LOW){
      delay(100);
      inMenu = true;
      button_in_menu = 1;
      showMenu();
    }
  }
}