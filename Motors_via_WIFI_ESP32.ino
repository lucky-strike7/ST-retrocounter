#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

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
int motorDiskSpeedRPM = 10;
int motorCounterSpeedRPM = 10;
int stepsPerDigit = 7000;

// =========================
// ПИНЫ
// =========================
// Мотор диска
const int diskPins[4] = {5, 18, 19, 21};

// Мотор счетчика
const int counterPins[4] = {14, 27, 26, 25};

// =========================
// УПРАВЛЕНИЕ МОТОРАМИ
// Full-step последовательность под 28BYJ-48
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
    // Интервал одного шага в микросекундах
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
  float stepIntervalUs = 0;

  void applyPhase(int idx) {
    // Full-step
    // 0: 1000
    // 1: 0100
    // 2: 0010
    // 3: 0001
    digitalWrite(pins[0], idx == 0 ? HIGH : LOW);
    digitalWrite(pins[1], idx == 1 ? HIGH : LOW);
    digitalWrite(pins[2], idx == 2 ? HIGH : LOW);
    digitalWrite(pins[3], idx == 3 ? HIGH : LOW);
  }
};

Simple28BYJ motorDisk;
Simple28BYJ motorCounter;

// =========================
// WEB SERVER / PREFS
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
int queuedDigitCount = 0;

bool motorsBusy = false;
String statusText = "Система запущена. Ожидание команды.";

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
  motorDiskSpeedRPM = prefs.getInt("diskRPM", 10);
  motorCounterSpeedRPM = prefs.getInt("countRPM", 10);
  stepsPerDigit = prefs.getInt("stepsDigit", 7168);

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
// HTML
// =========================
String htmlPage() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Motor Control</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      max-width: 1100px;
      margin: 24px auto;
      padding: 0 16px;
      background: #f5f5f5;
      color: #222;
    }
    .card {
      background: white;
      border-radius: 14px;
      padding: 20px;
      box-shadow: 0 4px 18px rgba(0,0,0,0.08);
      margin-bottom: 18px;
    }
    h1, h2 {
      margin-top: 0;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 14px;
    }
    label {
      display: block;
      font-size: 14px;
      color: #444;
      margin-bottom: 6px;
    }
    input[type="number"] {
      width: 100%;
      box-sizing: border-box;
      padding: 10px 12px;
      border-radius: 8px;
      border: 1px solid #ccc;
      font-size: 16px;
    }
    .checks {
      display: flex;
      gap: 24px;
      flex-wrap: wrap;
      margin-top: 8px;
    }
    .checks label {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 16px;
      margin: 0;
    }
    .buttons {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 10px;
      margin-top: 14px;
    }
    button {
      border: 0;
      border-radius: 10px;
      padding: 14px 18px;
      font-size: 16px;
      cursor: pointer;
      color: white;
    }
    button:disabled {
      opacity: 0.6;
      cursor: not-allowed;
    }
    .save { background: #6a1b9a; }
    .forward { background: #2e7d32; }
    .backward { background: #1565c0; }
    .digit { background: #ef6c00; }
    .status-box {
      background: #111;
      color: #0f0;
      border-radius: 10px;
      padding: 16px;
      min-height: 180px;
      white-space: pre-wrap;
      font-family: Consolas, monospace;
      font-size: 15px;
    }
    .small {
      color: #666;
      font-size: 13px;
      margin-top: 10px;
    }
  </style>
</head>
<body>
  <div class="card">
    <h1>Управление моторами</h1>

    <div class="grid">
      <div>
        <label for="stepsPerRevolution">Шагов на оборот</label>
        <input id="stepsPerRevolution" type="number" min="1" value=")rawliteral";
  html += String(stepsPerRevolution);
  html += R"rawliteral(">
      </div>

      <div>
        <label for="motorDiskSpeedRPM">Скорость диска (RPM)</label>
        <input id="motorDiskSpeedRPM" type="number" min="1" value=")rawliteral";
  html += String(motorDiskSpeedRPM);
  html += R"rawliteral(">
      </div>

      <div>
        <label for="motorCounterSpeedRPM">Скорость счетчика (RPM)</label>
        <input id="motorCounterSpeedRPM" type="number" min="1" value=")rawliteral";
  html += String(motorCounterSpeedRPM);
  html += R"rawliteral(">
      </div>

      <div>
        <label for="stepsPerDigit">Шагов на 1 цифру счетчика</label>
        <input id="stepsPerDigit" type="number" min="1" value=")rawliteral";
  html += String(stepsPerDigit);
  html += R"rawliteral(">
      </div>
    </div>

    <div class="checks">
      <label>
        <input id="checkCounter" type="checkbox" )rawliteral";
  if (uiSelection.counter) html += "checked";
  html += R"rawliteral(>
        Счетчик
      </label>

      <label>
        <input id="checkDisk" type="checkbox" )rawliteral";
  if (uiSelection.disk) html += "checked";
  html += R"rawliteral(>
        Диск
      </label>
    </div>

    <div class="buttons">
      <button class="save" onclick="saveSettingsToESP()">Сохранить настройки</button>
      <button class="save" onclick="pollYoutubeNow()">Обновить YouTube сейчас</button>
    </div>

    <div class="small">Чекбоксы влияют на кнопки прокрутки оборотами. Кнопки по цифрам крутят только мотор счетчика.</div>
  </div>

  <div class="card">
    <h2>Прокрутка выбранных моторов</h2>
    <div class="buttons">
      <button class="forward" onclick="rotateTurns(10)">Вперед 10 оборотов</button>
      <button class="backward" onclick="rotateTurns(-10)">Назад 10 оборотов</button>

      <button class="forward" onclick="rotateTurns(5)">Вперед 5 оборотов</button>
      <button class="backward" onclick="rotateTurns(-5)">Назад 5 оборотов</button>

      <button class="forward" onclick="rotateTurns(1)">Вперед 1 оборот</button>
      <button class="backward" onclick="rotateTurns(-1)">Назад 1 оборот</button>

      <button class="forward" onclick="rotateTurns(0.5)">Вперед 1/2 оборота</button>
      <button class="backward" onclick="rotateTurns(-0.5)">Назад 1/2 оборота</button>

      <button class="forward" onclick="rotateTurns(0.25)">Вперед 1/4 оборота</button>
      <button class="backward" onclick="rotateTurns(-0.25)">Назад 1/4 оборота</button>

      <button class="forward" onclick="rotateTurns(0.1)">Вперед 1/10 оборота</button>
      <button class="backward" onclick="rotateTurns(-0.1)">Назад 1/10 оборота</button>
    </div>
  </div>

  <div class="card">
    <h2>Прокрутка счетчика по цифрам</h2>
    <div class="buttons">
      <button class="digit" onclick="digitMove(1)">+1 цифра</button>
      <button class="digit" onclick="digitMove(-1)">-1 цифра</button>

      <button class="digit" onclick="digitMove(3)">+3 цифры</button>
      <button class="digit" onclick="digitMove(-3)">-3 цифры</button>

      <button class="digit" onclick="digitMove(5)">+5 цифр</button>
      <button class="digit" onclick="digitMove(-5)">-5 цифр</button>

      <button class="digit" onclick="digitMove(10)">+10 цифр</button>
      <button class="digit" onclick="digitMove(-10)">-10 цифр</button>
    </div>
    <div class="small">Эти кнопки крутят только мотор счетчика.</div>
  </div>

  <div class="card">
    <h2>Статус</h2>
    <div id="status" class="status-box">Загрузка статуса...</div>
  </div>

  <script>
    function getPayload() {
      return {
        stepsPerRevolution: parseInt(document.getElementById('stepsPerRevolution').value, 10),
        motorDiskSpeedRPM: parseInt(document.getElementById('motorDiskSpeedRPM').value, 10),
        motorCounterSpeedRPM: parseInt(document.getElementById('motorCounterSpeedRPM').value, 10),
        stepsPerDigit: parseInt(document.getElementById('stepsPerDigit').value, 10),
        counter: document.getElementById('checkCounter').checked,
        disk: document.getElementById('checkDisk').checked
      };
    }

    async function saveSettingsToESP() {
      try {
        const response = await fetch('/settings', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify(getPayload())
        });
        const text = await response.text();
        document.getElementById('status').textContent = text;
      } catch (e) {
        document.getElementById('status').textContent = 'Ошибка сохранения: ' + e;
      }
    }

    async function rotateTurns(turns) {
      try {
        const payload = getPayload();
        payload.turns = turns;

        const response = await fetch('/rotate', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify(payload)
        });

        const text = await response.text();
        document.getElementById('status').textContent = text;
      } catch (e) {
        document.getElementById('status').textContent = 'Ошибка отправки команды: ' + e;
      }
    }

    async function digitMove(count) {
      try {
        const payload = getPayload();
        payload.digitCount = count;

        const response = await fetch('/digitMove', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify(payload)
        });

        const text = await response.text();
        document.getElementById('status').textContent = text;
      } catch (e) {
        document.getElementById('status').textContent = 'Ошибка отправки команды: ' + e;
      }
    }

    async function pollYoutubeNow() {
      try {
        const response = await fetch('/youtubeNow', { method: 'POST' });
        const text = await response.text();
        document.getElementById('status').textContent = text;
      } catch (e) {
        document.getElementById('status').textContent = 'Ошибка запроса YouTube: ' + e;
      }
    }

    async function updateStatus() {
      try {
        const response = await fetch('/status');
        const text = await response.text();
        document.getElementById('status').textContent = text;

        const busy = text.includes('выполняется') || text.includes('крутятся');
        document.querySelectorAll('button').forEach(btn => btn.disabled = busy);
      } catch (e) {
        document.getElementById('status').textContent = 'Ошибка чтения статуса: ' + e;
      }
    }

    updateStatus();
    setInterval(updateStatus, 1000);
  </script>
</body>
</html>
)rawliteral";

  return html;
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

void rotateSimultaneous(long steps, bool useCounter, bool useDisk) {
  if (!useCounter && !useDisk) return;

  motorsBusy = true;

  String dirText = (steps >= 0) ? "вперед" : "назад";
  long totalSteps = labs(steps);
  int direction = (steps >= 0) ? 1 : -1;

  statusText = "Моторы крутятся.\n";
  statusText += "Направление: " + dirText + "\n";
  statusText += "Шагов: " + String(steps) + "\n";
  statusText += "Моторы: ";
  if (useCounter) statusText += "[Счетчик] ";
  if (useDisk) statusText += "[Диск] ";
  Serial.println(statusText);

  long doneCounter = 0;
  long doneDisk = 0;

  uint32_t lastCounterUs = micros();
  uint32_t lastDiskUs = micros();

  while ((useCounter && doneCounter < totalSteps) || (useDisk && doneDisk < totalSteps)) {
    uint32_t now = micros();

    if (useCounter && doneCounter < totalSteps) {
      if ((uint32_t)(now - lastCounterUs) >= (uint32_t)motorCounter.getStepIntervalUs()) {
        motorCounter.singleStep(direction);
        doneCounter++;
        lastCounterUs = now;
      }
    }

    if (useDisk && doneDisk < totalSteps) {
      if ((uint32_t)(now - lastDiskUs) >= (uint32_t)motorDisk.getStepIntervalUs()) {
        motorDisk.singleStep(direction);
        doneDisk++;
        lastDiskUs = now;
      }
    }

    delayMicroseconds(50);
  }

  if (useCounter) motorCounter.release();
  if (useDisk) motorDisk.release();

  statusText = "Готово.\n";
  statusText += "Сделано шагов: " + String(steps) + "\n";
  statusText += "Моторы: ";
  if (useCounter) statusText += "[Счетчик] ";
  if (useDisk) statusText += "[Диск] ";
  Serial.println(statusText);

  motorsBusy = false;
}

void rotateCounterDigit(long steps, int digitCount) {
  motorsBusy = true;

  String dirText = (steps >= 0) ? "вперед" : "назад";

  statusText = "Команда выполняется: сдвиг счетчика.\n";
  statusText += "Направление: " + dirText + "\n";
  statusText += "Цифр: " + String(digitCount) + "\n";
  statusText += "Шагов на 1 цифру: " + String(stepsPerDigit) + "\n";
  statusText += "Всего шагов: " + String(steps) + "\n";
  Serial.println(statusText);

  rotateSimultaneous(steps, true, false);

  statusText = "Готово: счетчик сдвинут.\n";
  statusText += "Цифр: " + String(digitCount) + "\n";
  statusText += "Сделано шагов: " + String(steps);
  Serial.println(statusText);

  motorsBusy = false;
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

  DynamicJsonDocument doc(2048);
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

  // При изменении подписчиков крутим счетчик всегда.
  // Диск крутится, только если отмечен чекбокс.
  rotateSimultaneous(steps, true, uiSelection.disk);

  lastSubscriberCount = newCount;
  youtubeStatus = "YouTube: значение обновлено, моторы прокручены";
  Serial.println(youtubeStatus);
}

// =========================
// HTTP HANDLERS
// =========================
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", htmlPage());
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

void handleRotate() {
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

  if (!uiSelection.counter && !uiSelection.disk) {
    statusText = "Команда не выполнена: не выбран ни один мотор.";
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
  queuedCommand.useCounter = uiSelection.counter;
  queuedCommand.useDisk = uiSelection.disk;
  pendingCommand = CMD_ROTATE_SELECTED;

  statusText = "Команда принята. Ожидание запуска.\nШагов: " + String(steps);
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

  int digitCount = parseJsonInt(body, "digitCount", 0);

  if (digitCount == 0) {
    statusText = "Команда не выполнена: digitCount = 0.";
    server.send(400, "text/plain; charset=utf-8", makeStatusMessage());
    return;
  }

  queuedDigitCount = digitCount;
  pendingCommand = CMD_DIGIT_MOVE;

  statusText = "Команда принята: сдвиг счетчика.\n";
  statusText += "Цифр: " + String(digitCount);
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

  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/settings", HTTP_POST, handleSettings);
  server.on("/rotate", HTTP_POST, handleRotate);
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
      rotateSimultaneous(
        queuedCommand.steps,
        queuedCommand.useCounter,
        queuedCommand.useDisk
      );
    } else if (cmd == CMD_DIGIT_MOVE) {
      rotateCounterDigit((long)stepsPerDigit * (long)queuedDigitCount, queuedDigitCount);
    }
  }

  processYoutubeUpdate(false);
}
