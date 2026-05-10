#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <math.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPIFFS.h>

// 0 = нет, 1 = LittleFS, 2 = SPIFFS (схема разделов «with spiffs» + классическая загрузка data)
static uint8_t g_dataFs = 0;

// =========================
// WIFI
// =========================
const char* ssid = "MTS_GPON_51C3";
const char* password = "fHYT7ekA";

// =========================
// НАСТРОЙКИ МОТОРОВ
// =========================
int stepsPerRevolution = 2048;
int motorDiskSpeedRPM = 5;
int motorCounterSpeedRPM = 16;
int stepsPerDigit = 6600;

// Ведомый диск: за stepsPerDigit шагов счётчика — ровно stepsPerRevolution шагов диска (1 оборот диска на 1 цифру)

// Кнопки 1/10 и 1/4: фиксированное число шагов мотора (не от stepsPerDigit)
const int kStepMoveOneTenth = 150;
const int kStepMoveOneFourth = 500;

// =========================
// ПИНЫ
// =========================
const int diskPins[4] = {5, 18, 19, 21};
const int counterPins[4] = {14, 27, 26, 25};

// «+» в интерфейсе крутит назад, «−» вперёд — чаще всего другой порядок 4 проводов на ULN2003,
// а не «перепутаны GPIO». true инвертирует направление шагов счётчика (и диска-ведома при связке).
const bool invertCounterMotorDirection = true;

// =========================
// УПРАВЛЕНИЕ МОТОРАМИ
// =========================
class Simple28BYJ {
public:
  Simple28BYJ() {}

  void begin(int p1, int p2, int p3, int p4) {
    pins[0] = p1;
    pins[1] = p2;
    pins[2] = p3;
    pins[3] = p4;

    for (int i = 0; i < 4; i++) {
      pinMode(pins[i], OUTPUT);
      digitalWrite(pins[i], LOW);
    }

    phaseIndex = 0;
    setSpeedRPM(10);
    release();
  }

  void setSpeedRPM(int rpmValue) {
    rpm = max(1, rpmValue);
    stepIntervalUs = (60.0f * 1000000.0f) / ((float)rpm * (float)max(1, stepsPerRevolution));
  }

  float getStepIntervalUs() const {
    return stepIntervalUs;
  }

  void singleStep(int direction) {
    if (direction > 0) {
      phaseIndex = (phaseIndex + 1) % 4;
    } else {
      phaseIndex = (phaseIndex + 3) % 4;
    }
    applyPhase(phaseIndex);
  }

  void release() {
    for (int i = 0; i < 4; i++) {
      digitalWrite(pins[i], LOW);
    }
  }

private:
  int pins[4] = {0, 0, 0, 0};
  int phaseIndex = 0;
  int rpm = 10;
  float stepIntervalUs = 0.0f;

  void applyPhase(int idx) {
    digitalWrite(pins[0], idx == 0 ? HIGH : LOW);
    digitalWrite(pins[1], idx == 1 ? HIGH : LOW);
    digitalWrite(pins[2], idx == 2 ? HIGH : LOW);
    digitalWrite(pins[3], idx == 3 ? HIGH : LOW);
  }
};

Simple28BYJ motorDisk;
Simple28BYJ motorCounter;

// =========================
// WEB / PREFS
// =========================
WebServer server(80);
Preferences prefs;

// =========================
// СОСТОЯНИЕ
// =========================
struct MotorSelection {
  bool counter = true;
  bool disk = false;
};

MotorSelection uiSelection;

enum CommandState {
  CMD_NONE,
  CMD_ROTATE_SELECTED,
  CMD_DIGIT_MOVE
};

volatile CommandState pendingCommand = CMD_NONE;

struct RotateCommand {
  long steps = 0;
  bool useCounter = true;
  bool useDisk = false;
};

RotateCommand queuedCommand;
float queuedDigitCountFloat = 0.0f;

bool motorsBusy = false;
String statusText = "Система запущена. Ожидание команды.";

float g_lastMoveDigits = 0.0f;
long g_lastMoveSteps = 0;

void recordLastMove(float digits, long steps) {
  g_lastMoveDigits = digits;
  g_lastMoveSteps = steps;
}

// =========================
// ЗНАЧЕНИЕ СЧЁТЧИКА (источник данных — ваш модуль)
// =========================
// g_logicCounter — логическое значение (double: учитываются дробные шаги 1/10 и 1/4 из веба).
// NAN до первого notifyCounterValue(). Тогда первый вызов только задаёт базу без прокрутки.
static double g_logicCounter = NAN;
long currentCounterValue = -1;
String dataSourceStatus;

// Текущее движение (для UI: вращение риски)
static long g_activeMoveSteps = 0;
static float g_activeDigitCount = 0.0f;

static long computeDigitMoveSteps(float digitCountF);

static long computeActiveStepsForApi() {
  if (motorsBusy) return g_activeMoveSteps;
  if (pendingCommand == CMD_DIGIT_MOVE) {
    return computeDigitMoveSteps(queuedDigitCountFloat);
  }
  if (pendingCommand == CMD_ROTATE_SELECTED && queuedCommand.useCounter) {
    return queuedCommand.steps;
  }
  return 0;
}

static float activeDigitsForApi() {
  if (motorsBusy) return g_activeDigitCount;
  if (pendingCommand == CMD_DIGIT_MOVE) return queuedDigitCountFloat;
  if (pendingCommand == CMD_ROTATE_SELECTED && queuedCommand.useCounter) {
    return (float)queuedCommand.steps / (float)max(1, stepsPerDigit);
  }
  return 0.0f;
}

// =========================
// СОХРАНЕНИЕ / ЗАГРУЗКА
// =========================
void loadSettings() {
  prefs.begin("countercfg", true);

  stepsPerRevolution = prefs.getInt("stepsRev", 2048);
  motorDiskSpeedRPM = prefs.getInt("diskRPM", 5);
  motorCounterSpeedRPM = prefs.getInt("countRPM", 16);
  stepsPerDigit = prefs.getInt("stepsDigit", 6600);

  uiSelection.counter = prefs.getBool("selCounter", true);
  uiSelection.disk = prefs.getBool("selDisk", false);

  prefs.end();
}

void saveSettings() {
  prefs.begin("countercfg", false);

  prefs.putInt("stepsRev", stepsPerRevolution);
  prefs.putInt("diskRPM", motorDiskSpeedRPM);
  prefs.putInt("countRPM", motorCounterSpeedRPM);
  prefs.putInt("stepsDigit", stepsPerDigit);

  prefs.putBool("selCounter", uiSelection.counter);
  prefs.putBool("selDisk", uiSelection.disk);

  prefs.end();
}

// =========================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// =========================
String makeStatusMessage() {
  String msg;
  msg += "IP: ";
  msg += WiFi.localIP().toString();
  msg += "\n";
  msg += "Шагов на оборот: " + String(stepsPerRevolution);
  msg += "\n";
  msg += "Скорость диска: " + String(motorDiskSpeedRPM) + " RPM";
  msg += "\n";
  msg += "Скорость счетчика: " + String(motorCounterSpeedRPM) + " RPM";
  msg += "\n";
  msg += "Шагов на 1 цифру: " + String(stepsPerDigit);
  msg += "\n";
  msg += "Выбрано: ";
  if (uiSelection.counter) msg += "[Счетчик] ";
  if (uiSelection.disk) msg += "[Диск] ";
  if (!uiSelection.counter && !uiSelection.disk) msg += "[ничего]";
  msg += "\n";
  msg += "Текущее значение: ";
  msg += (currentCounterValue >= 0 ? String(currentCounterValue) : String("не задано"));
  msg += "\n";
  if (dataSourceStatus.length() > 0) {
    msg += dataSourceStatus;
    msg += "\n";
  }
  msg += "Статус: " + statusText;
  return msg;
}

String getJsonValue(const String& body, const String& key) {
  String pattern = "\"" + key + "\"";
  int keyPos = body.indexOf(pattern);
  if (keyPos < 0) return "";

  int colonPos = body.indexOf(':', keyPos);
  if (colonPos < 0) return "";

  int start = colonPos + 1;
  while (start < (int)body.length() && (body[start] == ' ' || body[start] == '\"')) start++;

  int end = start;
  bool inString = false;

  if (start > 0 && body[start - 1] == '\"') {
    inString = true;
  }

  if (inString) {
    end = body.indexOf('\"', start);
    if (end < 0) return "";
    return body.substring(start, end);
  }

  while (end < (int)body.length()) {
    char c = body[end];
    if (c == ',' || c == '}' || c == '\n' || c == '\r') break;
    end++;
  }

  String value = body.substring(start, end);
  value.trim();
  return value;
}

bool parseJsonBool(const String& body, const String& key, bool defaultValue) {
  String value = getJsonValue(body, key);
  if (value == "true") return true;
  if (value == "false") return false;
  return defaultValue;
}

int parseJsonInt(const String& body, const String& key, int defaultValue) {
  String value = getJsonValue(body, key);
  if (value.length() == 0) return defaultValue;
  return value.toInt();
}

float parseJsonFloat(const String& body, const String& key, float defaultValue) {
  String value = getJsonValue(body, key);
  if (value.length() == 0) return defaultValue;
  return value.toFloat();
}

void rebuildMotors() {
  motorDisk.begin(diskPins[0], diskPins[1], diskPins[2], diskPins[3]);
  motorCounter.begin(counterPins[0], counterPins[1], counterPins[2], counterPins[3]);

  motorDisk.setSpeedRPM(motorDiskSpeedRPM);
  motorCounter.setSpeedRPM(motorCounterSpeedRPM);
}

// Пока моторы крутятся в блокирующем цикле, обрабатываем HTTP — иначе /api/state
// не отвечает до конца движения и веб-интерфейс не видит busy/activeSteps.
static void pumpWebWhileStepping(uint32_t &lastWebMs) {
  uint32_t m = millis();
  if (m - lastWebMs >= 25) {
    lastWebMs = m;
    server.handleClient();
    yield();
  }
}

void applySettingsFromBody(const String& body) {
  int newSteps = parseJsonInt(body, "stepsPerRevolution", stepsPerRevolution);
  int newDiskRPM = parseJsonInt(body, "motorDiskSpeedRPM", motorDiskSpeedRPM);
  int newCounterRPM = parseJsonInt(body, "motorCounterSpeedRPM", motorCounterSpeedRPM);
  int newStepsPerDigit = parseJsonInt(body, "stepsPerDigit", stepsPerDigit);

  bool newCounter = parseJsonBool(body, "counter", uiSelection.counter);
  bool newDisk = parseJsonBool(body, "disk", uiSelection.disk);

  if (newSteps > 0) stepsPerRevolution = newSteps;
  if (newDiskRPM > 0) motorDiskSpeedRPM = newDiskRPM;
  if (newCounterRPM > 0) motorCounterSpeedRPM = newCounterRPM;
  if (newStepsPerDigit > 0) stepsPerDigit = newStepsPerDigit;

  uiSelection.counter = newCounter;
  uiSelection.disk = newDisk;

  rebuildMotors();
}

// =========================
// ВРАЩЕНИЕ
// =========================

// Отдельное вращение диска
void rotateDiskOnly(long steps) {
  if (steps == 0) return;

  motorsBusy = true;
  g_activeMoveSteps = 0;

  String dirText = (steps >= 0) ? "вперед" : "назад";
  long totalSteps = labs(steps);
  int direction = (steps >= 0) ? 1 : -1;

  statusText = "Диск крутится отдельно.\n";
  statusText += "Направление: " + dirText + "\n";
  statusText += "Шагов: " + String(steps) + "\n";
  Serial.println(statusText);

  uint32_t lastDiskUs = micros();
  long doneDisk = 0;
  uint32_t lastWebMs = millis();

  while (doneDisk < totalSteps) {
    uint32_t now = micros();
    if ((uint32_t)(now - lastDiskUs) >= (uint32_t)motorDisk.getStepIntervalUs()) {
      motorDisk.singleStep(direction);
      doneDisk++;
      lastDiskUs = now;
    }
    delayMicroseconds(50);
    pumpWebWhileStepping(lastWebMs);
  }

  motorDisk.release();

  statusText = "Готово: диск прокручен отдельно.\n";
  statusText += "Сделано шагов: " + String(steps);
  Serial.println(statusText);

  recordLastMove(0.0f, steps);

  motorsBusy = false;
  g_activeMoveSteps = 0;
  g_activeDigitCount = 0.0f;
}

// Счетчик ведущий, диск ведомый: 1 шаг диска на каждые 4 шага счетчика
void rotateCounterWithOptionalDisk(long counterSteps, bool useDiskFollower) {
  if (counterSteps == 0) return;

  motorsBusy = true;
  g_activeMoveSteps = counterSteps;

  String dirText = (counterSteps >= 0) ? "вперед" : "назад";
  long totalCounterSteps = labs(counterSteps);
  int direction = (counterSteps >= 0) ? 1 : -1;
  const int stepDir = invertCounterMotorDirection ? -direction : direction;

  statusText = "Моторы крутятся.\n";
  statusText += "Направление: " + dirText + "\n";
  statusText += "Шагов счетчика: " + String(counterSteps) + "\n";
  statusText += "Моторы: [Счетчик] ";
  if (useDiskFollower) statusText += "[Диск 1 об/цифру] ";
  Serial.println(statusText);

  uint32_t lastCounterUs = micros();
  long doneCounter = 0;
  long diskStepAccum = 0;
  const int spdFollow = max(1, stepsPerDigit);
  const int sprFollow = max(1, stepsPerRevolution);
  uint32_t lastWebMs = millis();

  while (doneCounter < totalCounterSteps) {
    uint32_t now = micros();

    if ((uint32_t)(now - lastCounterUs) >= (uint32_t)motorCounter.getStepIntervalUs()) {
      motorCounter.singleStep(stepDir);
      doneCounter++;
      lastCounterUs = now;

      if (useDiskFollower) {
        diskStepAccum += sprFollow;
        while (diskStepAccum >= spdFollow) {
          motorDisk.singleStep(stepDir);
          diskStepAccum -= spdFollow;
        }
      }
    }

    delayMicroseconds(50);
    pumpWebWhileStepping(lastWebMs);
  }

  motorCounter.release();
  if (useDiskFollower) motorDisk.release();

  statusText = "Готово.\n";
  statusText += "Сделано шагов счетчика: " + String(counterSteps) + "\n";
  statusText += "Моторы: [Счетчик] ";
  if (useDiskFollower) statusText += "[Диск] ";
  Serial.println(statusText);

  motorsBusy = false;
  g_activeMoveSteps = 0;
  g_activeDigitCount = 0.0f;
}

void rotateCounterDigit(long steps, float digitCountDisplay) {
  g_activeDigitCount = digitCountDisplay;

  String dirText = (steps >= 0) ? "вперед" : "назад";

  statusText = "Команда выполняется: сдвиг счетчика.\n";
  statusText += "Направление: " + dirText + "\n";
  statusText += "Цифр: " + String(digitCountDisplay, 4) + "\n";
  statusText += "Шагов на 1 цифру: " + String(stepsPerDigit) + "\n";
  statusText += "Всего шагов: " + String(steps) + "\n";
  statusText += "Моторы: [Счетчик] ";
  if (uiSelection.disk) statusText += "[Диск] ";
  Serial.println(statusText);

  rotateCounterWithOptionalDisk(steps, uiSelection.disk);

  recordLastMove(digitCountDisplay, steps);

  if (!isnan(g_logicCounter)) {
    g_logicCounter += (double)digitCountDisplay;
    currentCounterValue = (long)lround(g_logicCounter);
  }

  statusText = "Готово: счетчик сдвинут.\n";
  statusText += "Цифр: " + String(digitCountDisplay, 4) + "\n";
  statusText += "Сделано шагов: " + String(steps) + "\n";
  statusText += "Моторы: [Счетчик] ";
  if (uiSelection.disk) statusText += "[Диск] ";
  Serial.println(statusText);
}

// =========================
// ИНТЕГРАЦИЯ: новое значение счётчика с вашего источника
// =========================
// Первый вызов задаёт базу без движения; далее моторы крутятся на (newValue - база) в единицах «цифр» * stepsPerDigit.
// Если моторы заняты или в очереди команда из веба — вызов игнорируется (см. Serial).
void notifyCounterValue(long newValue) {
  if (motorsBusy || pendingCommand != CMD_NONE) {
    Serial.println("[counter] notifyCounterValue: пропуск — моторы заняты или очередь команд");
    return;
  }

  if (isnan(g_logicCounter)) {
    g_logicCounter = (double)newValue;
    currentCounterValue = newValue;
    statusText = "Первое значение: " + String(newValue) + " (моторы не крутились).";
    Serial.println(statusText);
    return;
  }

  double diffD = (double)newValue - g_logicCounter;
  if (fabs(diffD) < 1e-6) {
    g_logicCounter = (double)newValue;
    currentCounterValue = newValue;
    return;
  }

  if (!uiSelection.counter) {
    Serial.println("[counter] notifyCounterValue: счётчик выключен в UI — вызов проигнорирован");
    return;
  }

  long oldDisp = (long)lround(g_logicCounter);
  long steps = lround(diffD * (double)stepsPerDigit);

  statusText = "Данные: " + String(oldDisp) + " -> " + String(newValue) + "\n";
  statusText += "Разница (цифр): " + String((float)diffD, 4) + "\n";
  statusText += "Шагов: " + String(steps);

  g_activeDigitCount = (float)diffD;
  rotateCounterWithOptionalDisk(steps, uiSelection.disk);

  recordLastMove((float)diffD, steps);

  g_logicCounter = (double)newValue;
  currentCounterValue = newValue;
}

// Строка для подписи в веб-интерфейсе (произвольный текст от вашего модуля, необязательно).
void setDataSourceStatus(const String& line) {
  dataSourceStatus = line;
}

// =========================
// HTTP HANDLERS
// =========================
static File openDataFile(const char* path) {
  if (g_dataFs == 1) return LittleFS.open(path, "r");
  if (g_dataFs == 2) return SPIFFS.open(path, "r");
  return File();
}

static bool streamDataFile(const char* path, const char* mime) {
  File f = openDataFile(path);
  if (!f) return false;
  server.streamFile(f, mime);
  f.close();
  return true;
}

void handleNewRoot() {
  File f = openDataFile("/index.html");
  if (!f) {
    f = openDataFile("/preview.html");
  }
  if (!f) {
    server.send(
      500, "text/plain; charset=utf-8",
      "Нет index.html в файловой системе.\r\n"
      "1) Папка data/: index.html, style.css, script.js\r\n"
      "2) Arduino: Инструменты -> ESP32 Sketch Data Upload (или Upload SPIFFS)\r\n"
      "3) При схеме \"with spiffs\" данные часто в SPIFFS — прошивка поддерживает и SPIFFS, и LittleFS.\r\n"
      "4) После загрузки data перезагрузите ESP.");
    return;
  }
  server.streamFile(f, "text/html; charset=utf-8");
  f.close();
}

void handleStyleCss() {
  if (!streamDataFile("/style.css", "text/css; charset=utf-8")) {
    server.send(404, "text/plain; charset=utf-8", "style.css not found");
  }
}

void handleScriptJs() {
  if (!streamDataFile("/script.js", "application/javascript; charset=utf-8")) {
    server.send(404, "text/plain; charset=utf-8", "script.js not found");
  }
}

void handleApiState() {
  JsonDocument doc;
  doc["busy"] = motorsBusy || (pendingCommand != CMD_NONE);
  doc["counterEnabled"] = uiSelection.counter;
  doc["diskEnabled"] = uiSelection.disk;
  doc["motorCounterSpeedRPM"] = motorCounterSpeedRPM;
  doc["motorDiskSpeedRPM"] = motorDiskSpeedRPM;
  doc["stepsPerDigit"] = stepsPerDigit;
  doc["stepsPerRevolution"] = stepsPerRevolution;
  doc["counterValue"] = currentCounterValue;
  doc["dataSourceLine"] = dataSourceStatus;
  doc["statusLine"] = statusText;
  doc["lastDigits"] = g_lastMoveDigits;
  doc["lastSteps"] = g_lastMoveSteps;
  doc["activeSteps"] = computeActiveStepsForApi();
  doc["activeDigits"] = activeDigitsForApi();

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json; charset=utf-8", json);
}

void handleStatus() {
  server.send(200, "text/plain; charset=utf-8", makeStatusMessage());
}

void handleSettings() {
  if (!server.hasArg("plain")) {
    server.send(400, "text/plain; charset=utf-8", "Нет тела запроса.");
    return;
  }

  String body = server.arg("plain");
  applySettingsFromBody(body);
  saveSettings();

  statusText = "Настройки сохранены.";
  server.send(200, "text/plain; charset=utf-8", makeStatusMessage());
}

void handleRotateCounter() {
  if (motorsBusy || pendingCommand != CMD_NONE) {
    server.send(409, "text/plain; charset=utf-8", makeStatusMessage() + "\nКоманда не принята: моторы заняты.");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "text/plain; charset=utf-8", "Нет тела запроса.");
    return;
  }

  String body = server.arg("plain");
  applySettingsFromBody(body);
  saveSettings();

  if (!uiSelection.counter) {
    statusText = "Команда не выполнена: счетчик выключен чекбоксом.";
    server.send(400, "text/plain; charset=utf-8", makeStatusMessage());
    return;
  }

  float turns = parseJsonFloat(body, "turns", 0.0f);
  long steps = lround(turns * stepsPerRevolution);

  if (steps == 0) {
    statusText = "Команда не выполнена: шагов получилось 0.";
    server.send(400, "text/plain; charset=utf-8", makeStatusMessage());
    return;
  }

  queuedCommand.steps = steps;
  queuedCommand.useCounter = true;
  queuedCommand.useDisk = uiSelection.disk;
  pendingCommand = CMD_ROTATE_SELECTED;

  statusText = "Команда принята. Ожидание запуска.\nШагов: " + String(steps);
  server.send(200, "text/plain; charset=utf-8", makeStatusMessage());
}

void handleRotateDisk() {
  if (motorsBusy || pendingCommand != CMD_NONE) {
    server.send(409, "text/plain; charset=utf-8", makeStatusMessage() + "\nКоманда не принята: моторы заняты.");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "text/plain; charset=utf-8", "Нет тела запроса.");
    return;
  }

  String body = server.arg("plain");
  applySettingsFromBody(body);
  saveSettings();

  float turns = parseJsonFloat(body, "turns", 0.0f);
  long steps = lround(turns * stepsPerRevolution);

  if (steps == 0) {
    statusText = "Команда не выполнена: шагов получилось 0.";
    server.send(400, "text/plain; charset=utf-8", makeStatusMessage());
    return;
  }

  queuedCommand.steps = steps;
  queuedCommand.useCounter = false;
  queuedCommand.useDisk = true;
  pendingCommand = CMD_ROTATE_SELECTED;

  statusText = "Команда принята: отдельное вращение диска.\nШагов: " + String(steps);
  server.send(200, "text/plain; charset=utf-8", makeStatusMessage());
}

void handleDigitMove() {
  if (motorsBusy || pendingCommand != CMD_NONE) {
    server.send(409, "text/plain; charset=utf-8", makeStatusMessage() + "\nКоманда не принята: моторы заняты.");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "text/plain; charset=utf-8", "Нет тела запроса.");
    return;
  }

  String body = server.arg("plain");
  applySettingsFromBody(body);
  saveSettings();

  if (!uiSelection.counter) {
    statusText = "Команда не выполнена: счетчик выключен чекбоксом.";
    server.send(400, "text/plain; charset=utf-8", makeStatusMessage());
    return;
  }

  float digitCountF = parseJsonFloat(body, "digitCount", 0.0f);

  if (fabs((double)digitCountF) < 1e-6) {
    statusText = "Команда не выполнена: digitCount = 0.";
    server.send(400, "text/plain; charset=utf-8", makeStatusMessage());
    return;
  }

  queuedDigitCountFloat = digitCountF;
  pendingCommand = CMD_DIGIT_MOVE;

  statusText = "Команда принята: сдвиг счетчика.\n";
  statusText += "Цифр: " + String(digitCountF, 4);
  server.send(200, "text/plain; charset=utf-8", makeStatusMessage());
}

void handleCounterValuePost() {
  if (motorsBusy || pendingCommand != CMD_NONE) {
    server.send(409, "text/plain; charset=utf-8", makeStatusMessage() + "\nКоманда не принята: моторы заняты.");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "text/plain; charset=utf-8", "Нет тела запроса.");
    return;
  }

  String body = server.arg("plain");
  String vStr = getJsonValue(body, "value");
  if (vStr.length() == 0) {
    server.send(400, "text/plain; charset=utf-8", "Ожидается JSON с полем \"value\" (целое число).");
    return;
  }

  long v = atol(vStr.c_str());
  notifyCounterValue(v);
  server.send(200, "text/plain; charset=utf-8", makeStatusMessage());
}

static long computeDigitMoveSteps(float digitCountF) {
  const double mag = fabs((double)digitCountF);
  const int sgn = (digitCountF >= 0.0f) ? 1 : -1;
  if (fabs(mag - 0.1) < 0.001) return (long)kStepMoveOneTenth * sgn;
  if (fabs(mag - 0.25) < 0.001) return (long)kStepMoveOneFourth * sgn;
  return (long)lround((double)stepsPerDigit * (double)digitCountF);
}

// =========================
// SETUP / LOOP
// =========================
void setup() {
  Serial.begin(115200);
  delay(500);

  loadSettings();
  rebuildMotors();

  Serial.println("Подключение к Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println("Wi-Fi подключен.");
  Serial.print("IP адрес: ");
  Serial.println(WiFi.localIP());

  // Стабильный доступ по имени в локальной сети (не зависит от смены DHCP IP)
  if (MDNS.begin("ST-retrocounter")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://ST-retrocounter.local");
  } else {
    Serial.println("mDNS: не удалось запустить");
  }

  statusText = "Wi-Fi подключен. Веб-интерфейс готов.";

  g_dataFs = 0;
  if (LittleFS.begin(false)) {
    g_dataFs = 1;
    Serial.println("[FS] LittleFS смонтирован.");
  } else if (SPIFFS.begin(false)) {
    g_dataFs = 2;
    Serial.println("[FS] SPIFFS смонтирован (часто при Partition Scheme \"with spiffs\").");
  } else {
    Serial.println("[FS] Не удалось смонтировать LittleFS или SPIFFS (begin false).");
    Serial.println("[FS] Проверьте схему разделов; при первой настройке может понадобиться загрузить скетч с однократным SPIFFS.format() в setup — см. документацию.");
  }
  if (g_dataFs != 0) {
    File test = openDataFile("/index.html");
    if (test) {
      test.close();
      Serial.println("[FS] index.html найден.");
    } else {
      Serial.println("[FS] index.html не найден — залейте папку data (index.html, style.css, script.js).");
    }
  }

  server.on("/", HTTP_GET, handleNewRoot);
  server.on("/style.css", HTTP_GET, handleStyleCss);
  server.on("/script.js", HTTP_GET, handleScriptJs);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/rotateCounter", HTTP_POST, handleRotateCounter);
  server.on("/rotateDisk", HTTP_POST, handleRotateDisk);
  server.on("/digitMove", HTTP_POST, handleDigitMove);
  server.on("/counterValue", HTTP_POST, handleCounterValuePost);

  server.begin();
  Serial.println("Веб-сервер запущен.");
}

void loop() {
  server.handleClient();

  if (!motorsBusy && pendingCommand != CMD_NONE) {
    CommandState cmd = pendingCommand;
    pendingCommand = CMD_NONE;

    if (cmd == CMD_ROTATE_SELECTED) {
      if (queuedCommand.useCounter) {
        g_activeDigitCount = (float)queuedCommand.steps / (float)max(1, stepsPerDigit);
        rotateCounterWithOptionalDisk(queuedCommand.steps, queuedCommand.useDisk);
        recordLastMove((float)queuedCommand.steps / (float)max(1, stepsPerDigit), queuedCommand.steps);
        if (!isnan(g_logicCounter)) {
          g_logicCounter += (double)queuedCommand.steps / (double)max(1, stepsPerDigit);
          currentCounterValue = (long)lround(g_logicCounter);
        }
      } else if (queuedCommand.useDisk) {
        rotateDiskOnly(queuedCommand.steps);
      }
    } else if (cmd == CMD_DIGIT_MOVE) {
      long steps = computeDigitMoveSteps(queuedDigitCountFloat);
      rotateCounterDigit(steps, queuedDigitCountFloat);
    }
  }
}
