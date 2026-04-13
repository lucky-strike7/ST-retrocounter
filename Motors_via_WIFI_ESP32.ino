#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
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
// YOUTUBE API
// =========================
const char* youtubeApiKey = "AIzaSyDc2sUfupIQBQKyaFiPDEuVgL0OBwYyAZo";
const char* youtubeChannelId = "UCFKBfVnPza1Xp8ymEwR-qzg";

// Интервал опроса YouTube
const unsigned long youtubePollIntervalMs = 15000;

// =========================
// НАСТРОЙКИ МОТОРОВ
// =========================
int stepsPerRevolution = 2048;
int motorDiskSpeedRPM = 5;
int motorCounterSpeedRPM = 16;
int stepsPerDigit = 6600;

// Ведомый диск: 1 шаг диска на каждые 4 шага счетчика
const int diskFollowRatio = 4;

// Кнопки 1/10 и 1/4: фиксированное число шагов мотора (не от stepsPerDigit)
const int kStepMoveOneTenth = 150;
const int kStepMoveOneFourth = 500;

// =========================
// ПИНЫ
// =========================
const int diskPins[4] = {5, 18, 19, 21};
const int counterPins[4] = {14, 27, 26, 25};

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
// YOUTUBE СОСТОЯНИЕ
// =========================
unsigned long lastYoutubePollMs = 0;
long lastSubscriberCount = -1;
long currentSubscriberCount = -1;
String youtubeStatus = "YouTube: ожидание первого запроса";

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
  msg += "Подписчики: ";
  msg += (currentSubscriberCount >= 0 ? String(currentSubscriberCount) : String("неизвестно"));
  msg += "\n";
  msg += youtubeStatus;
  msg += "\n";
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

  String dirText = (steps >= 0) ? "вперед" : "назад";
  long totalSteps = labs(steps);
  int direction = (steps >= 0) ? 1 : -1;

  statusText = "Диск крутится отдельно.\n";
  statusText += "Направление: " + dirText + "\n";
  statusText += "Шагов: " + String(steps) + "\n";
  Serial.println(statusText);

  uint32_t lastDiskUs = micros();
  long doneDisk = 0;

  while (doneDisk < totalSteps) {
    uint32_t now = micros();
    if ((uint32_t)(now - lastDiskUs) >= (uint32_t)motorDisk.getStepIntervalUs()) {
      motorDisk.singleStep(direction);
      doneDisk++;
      lastDiskUs = now;
    }
    delayMicroseconds(50);
  }

  motorDisk.release();

  statusText = "Готово: диск прокручен отдельно.\n";
  statusText += "Сделано шагов: " + String(steps);
  Serial.println(statusText);

  recordLastMove(0.0f, steps);

  motorsBusy = false;
}

// Счетчик ведущий, диск ведомый: 1 шаг диска на каждые 4 шага счетчика
void rotateCounterWithOptionalDisk(long counterSteps, bool useDiskFollower) {
  if (counterSteps == 0) return;

  motorsBusy = true;

  String dirText = (counterSteps >= 0) ? "вперед" : "назад";
  long totalCounterSteps = labs(counterSteps);
  int direction = (counterSteps >= 0) ? 1 : -1;

  statusText = "Моторы крутятся.\n";
  statusText += "Направление: " + dirText + "\n";
  statusText += "Шагов счетчика: " + String(counterSteps) + "\n";
  statusText += "Моторы: [Счетчик] ";
  if (useDiskFollower) statusText += "[Диск follows 1/4] ";
  Serial.println(statusText);

  uint32_t lastCounterUs = micros();
  long doneCounter = 0;
  long counterStepsSinceDisk = 0;

  while (doneCounter < totalCounterSteps) {
    uint32_t now = micros();

    if ((uint32_t)(now - lastCounterUs) >= (uint32_t)motorCounter.getStepIntervalUs()) {
      motorCounter.singleStep(direction);
      doneCounter++;
      counterStepsSinceDisk++;
      lastCounterUs = now;

      if (useDiskFollower && counterStepsSinceDisk >= diskFollowRatio) {
        motorDisk.singleStep(direction);
        counterStepsSinceDisk = 0;
      }
    }

    delayMicroseconds(50);
  }

  motorCounter.release();
  if (useDiskFollower) motorDisk.release();

  statusText = "Готово.\n";
  statusText += "Сделано шагов счетчика: " + String(counterSteps) + "\n";
  statusText += "Моторы: [Счетчик] ";
  if (useDiskFollower) statusText += "[Диск] ";
  Serial.println(statusText);

  motorsBusy = false;
}

void rotateCounterDigit(long steps, float digitCountDisplay) {
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

  statusText = "Готово: счетчик сдвинут.\n";
  statusText += "Цифр: " + String(digitCountDisplay, 4) + "\n";
  statusText += "Сделано шагов: " + String(steps) + "\n";
  statusText += "Моторы: [Счетчик] ";
  if (uiSelection.disk) statusText += "[Диск] ";
  Serial.println(statusText);
}

// =========================
// YOUTUBE
// =========================
bool fetchSubscriberCount(long &subscriberCountOut, String &errorText) {
  if (WiFi.status() != WL_CONNECTED) {
    errorText = "Wi-Fi не подключен";
    return false;
  }

  String url = "https://www.googleapis.com/youtube/v3/channels?part=statistics&id=";
  url += youtubeChannelId;
  url += "&fields=items/statistics/subscriberCount";
  url += "&key=";
  url += youtubeApiKey;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    errorText = "Не удалось открыть HTTPS соединение";
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    errorText = "HTTP ошибка: " + String(httpCode);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    errorText = "Ошибка JSON: " + String(err.c_str());
    return false;
  }

  JsonArray items = doc["items"].as<JsonArray>();
  if (items.isNull() || items.size() == 0) {
    errorText = "Канал не найден или ответ пустой";
    return false;
  }

  const char* countStr = items[0]["statistics"]["subscriberCount"];
  if (countStr == nullptr) {
    errorText = "subscriberCount не найден";
    return false;
  }

  subscriberCountOut = atol(countStr);
  return true;
}

void processYoutubeUpdate(bool forceNow = false) {
  if (!forceNow) {
    if (millis() - lastYoutubePollMs < youtubePollIntervalMs) return;
  }

  if (motorsBusy || pendingCommand != CMD_NONE) return;

  lastYoutubePollMs = millis();

  long newCount = -1;
  String errorText;

  youtubeStatus = "YouTube: выполняется запрос...";
  Serial.println(youtubeStatus);

  if (!fetchSubscriberCount(newCount, errorText)) {
    youtubeStatus = "YouTube: ошибка - " + errorText;
    Serial.println(youtubeStatus);
    return;
  }

  currentSubscriberCount = newCount;
  youtubeStatus = "YouTube: получено значение " + String(newCount);
  Serial.println(youtubeStatus);

  if (lastSubscriberCount < 0) {
    lastSubscriberCount = newCount;
    youtubeStatus = "YouTube: первое значение сохранено, без прокрутки";
    Serial.println(youtubeStatus);
    return;
  }

  long diff = newCount - lastSubscriberCount;
  if (diff == 0) {
    youtubeStatus = "YouTube: изменений нет";
    Serial.println(youtubeStatus);
    return;
  }

  long steps = (long)stepsPerDigit * diff;

  statusText = "Изменение подписчиков: " + String(lastSubscriberCount) + " -> " + String(newCount) + "\n";
  statusText += "Разница: " + String(diff) + "\n";
  statusText += "Прокрутка шагов: " + String(steps);

  rotateCounterWithOptionalDisk(steps, uiSelection.disk);

  recordLastMove((float)diff, steps);

  lastSubscriberCount = newCount;
  youtubeStatus = "YouTube: значение обновлено, моторы прокручены";
  Serial.println(youtubeStatus);
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
  doc["subscribers"] = currentSubscriberCount;
  doc["youtubeLine"] = youtubeStatus;
  doc["statusLine"] = statusText;
  doc["lastDigits"] = g_lastMoveDigits;
  doc["lastSteps"] = g_lastMoveSteps;

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

void handleYoutubeNow() {
  if (motorsBusy || pendingCommand != CMD_NONE) {
    server.send(409, "text/plain; charset=utf-8", makeStatusMessage() + "\nКоманда не принята: моторы заняты.");
    return;
  }

  processYoutubeUpdate(true);
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
  server.on("/youtubeNow", HTTP_POST, handleYoutubeNow);

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
        rotateCounterWithOptionalDisk(queuedCommand.steps, queuedCommand.useDisk);
        recordLastMove((float)queuedCommand.steps / (float)max(1, stepsPerDigit), queuedCommand.steps);
      } else if (queuedCommand.useDisk) {
        rotateDiskOnly(queuedCommand.steps);
      }
    } else if (cmd == CMD_DIGIT_MOVE) {
      long steps = computeDigitMoveSteps(queuedDigitCountFloat);
      rotateCounterDigit(steps, queuedDigitCountFloat);
    }
  }

  processYoutubeUpdate(false);
}
