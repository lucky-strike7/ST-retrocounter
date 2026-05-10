#!/usr/bin/env python3
"""
Локальный превью-сервер для веб-интерфейса (как на ESP32).
Запуск: python dev_server.py
С телефона: http://<IP_вашего_ПК>:8080/  (ПК и телефон в одной сети Wi-Fi).
"""
from __future__ import annotations

import json
import os
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

HOST = "0.0.0.0"
PORT = 8080

# Как в прошивке
K_STEP_MOVE_ONE_TENTH = 150
K_STEP_MOVE_ONE_FOURTH = 500


def compute_digit_move_steps(digit_count: float, steps_per_digit: int) -> int:
    mag = abs(digit_count)
    sgn = 1 if digit_count >= 0 else -1
    if abs(mag - 0.1) < 0.001:
        return K_STEP_MOVE_ONE_TENTH * sgn
    if abs(mag - 0.25) < 0.001:
        return K_STEP_MOVE_ONE_FOURTH * sgn
    return int(round(float(steps_per_digit) * float(digit_count)))


class MockState:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self.busy = False
        self.counter_enabled = True
        self.disk_enabled = False
        self.motor_counter_rpm = 16
        self.motor_disk_rpm = 5
        self.steps_per_digit = 6600
        self.steps_per_revolution = 2048
        self.counter_value = 12_345
        self.data_source_line = "Mock: свой источник данных (без ESP32)"
        self.status_text = "Mock-сервер. Моторы не крутятся."
        self.last_digits = 0.0
        self.last_steps = 0
        self.active_steps = 0
        self.active_digits = 0.0

    def apply_settings_body(self, body: dict) -> None:
        if "stepsPerRevolution" in body:
            v = int(body["stepsPerRevolution"])
            if v > 0:
                self.steps_per_revolution = v
        if "motorDiskSpeedRPM" in body:
            v = int(body["motorDiskSpeedRPM"])
            if v > 0:
                self.motor_disk_rpm = v
        if "motorCounterSpeedRPM" in body:
            v = int(body["motorCounterSpeedRPM"])
            if v > 0:
                self.motor_counter_rpm = v
        if "stepsPerDigit" in body:
            v = int(body["stepsPerDigit"])
            if v > 0:
                self.steps_per_digit = v
        if "counter" in body:
            self.counter_enabled = bool(body["counter"])
        if "disk" in body:
            self.disk_enabled = bool(body["disk"])

    def make_status_plain(self) -> str:
        sel = []
        if self.counter_enabled:
            sel.append("[Счетчик]")
        if self.disk_enabled:
            sel.append("[Диск]")
        sel_s = " ".join(sel) if sel else "[ничего]"
        return (
            f"IP: 127.0.0.1 (mock)\n"
            f"Шагов на оборот: {self.steps_per_revolution}\n"
            f"Скорость диска: {self.motor_disk_rpm} RPM\n"
            f"Скорость счетчика: {self.motor_counter_rpm} RPM\n"
            f"Шагов на 1 цифру: {self.steps_per_digit}\n"
            f"Выбрано: {sel_s}\n"
            f"Текущее значение: {self.counter_value}\n"
            f"{self.data_source_line}\n"
            f"Статус: {self.status_text}"
        )

    def api_state_dict(self) -> dict:
        with self._lock:
            return {
                "busy": self.busy,
                "counterEnabled": self.counter_enabled,
                "diskEnabled": self.disk_enabled,
                "motorCounterSpeedRPM": self.motor_counter_rpm,
                "motorDiskSpeedRPM": self.motor_disk_rpm,
                "stepsPerDigit": self.steps_per_digit,
                "stepsPerRevolution": self.steps_per_revolution,
                "counterValue": self.counter_value,
                "dataSourceLine": self.data_source_line,
                "statusLine": self.status_text,
                "lastDigits": self.last_digits,
                "lastSteps": self.last_steps,
                "activeSteps": self.active_steps,
                "activeDigits": self.active_digits,
            }

    def start_digit_move(self, digit_count: float) -> tuple[bool, str]:
        with self._lock:
            if self.busy:
                return False, self.make_status_plain() + "\nКоманда не принята: моторы заняты."
            if not self.counter_enabled:
                self.status_text = "Команда не выполнена: счетчик выключен чекбоксом."
                return True, self.make_status_plain()
            if abs(digit_count) < 1e-6:
                self.status_text = "Команда не выполнена: digitCount = 0."
                return True, self.make_status_plain()

            steps = compute_digit_move_steps(digit_count, self.steps_per_digit)
            if steps == 0:
                self.status_text = "Команда не выполнена: шагов получилось 0."
                return True, self.make_status_plain()

            self.busy = True
            self.active_steps = steps
            self.active_digits = float(digit_count)
            self.status_text = (
                f"Команда принята: сдвиг счетчика.\nЦифр: {digit_count:.4f}\nШагов: {steps}"
            )

            spr = max(1, self.steps_per_revolution)
            rpm = max(1, self.motor_counter_rpm)
            steps_per_sec = (rpm / 60.0) * spr
            duration = max(0.35, abs(steps) / steps_per_sec)
            # Укороченно для превью (не ждать минуты)
            duration = min(4.0, duration)

        def finish() -> None:
            time.sleep(duration)
            with self._lock:
                self.last_digits = float(digit_count)
                self.last_steps = steps
                self.busy = False
                self.active_steps = 0
                self.active_digits = 0.0
                self.status_text = (
                    f"Готово (mock): счетчик сдвинут.\nЦифр: {digit_count:.4f}\nШагов: {steps}"
                )

        threading.Thread(target=finish, daemon=True).start()
        return True, self.make_status_plain()


STATE = MockState()
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
MIME = {
    ".html": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".js": "application/javascript; charset=utf-8",
    ".ico": "image/x-icon",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".webp": "image/webp",
}


def read_body(handler: BaseHTTPRequestHandler) -> bytes:
    n = int(handler.headers.get("Content-Length", "0") or "0")
    if n <= 0:
        return b""
    return handler.rfile.read(n)


class Handler(BaseHTTPRequestHandler):
    server_version = "MockESP32/1.0"

    def log_message(self, fmt: str, *args) -> None:
        print(f"[{time.strftime('%H:%M:%S')}] {args[0]} {args[1]} {args[2]}")

    def send_json(self, obj: dict, code: int = 200) -> None:
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_text(self, text: str, code: int = 200) -> None:
        data = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_file(self, rel_url_path: str) -> None:
        path = urlparse(rel_url_path).path
        if path == "/" or path == "":
            fs_path = os.path.join(DATA_DIR, "index.html")
        else:
            safe = path.lstrip("/").replace("\\", "/")
            if ".." in safe.split("/"):
                self.send_error(400)
                return
            fs_path = os.path.join(DATA_DIR, safe)
        if not os.path.isfile(fs_path):
            self.send_error(404)
            return
        ext = os.path.splitext(fs_path)[1].lower()
        ctype = MIME.get(ext, "application/octet-stream")
        with open(fs_path, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        if self.path.startswith("/api/state"):
            self.send_json(STATE.api_state_dict())
            return
        if self.path.startswith("/status"):
            self.send_text(STATE.make_status_plain())
            return
        self.send_file(self.path)

    def do_POST(self) -> None:
        raw = read_body(self)
        try:
            body = json.loads(raw.decode("utf-8")) if raw else {}
        except json.JSONDecodeError:
            self.send_text("Неверный JSON.", 400)
            return

        if self.path.startswith("/settings"):
            with STATE._lock:
                STATE.apply_settings_body(body)
                STATE.status_text = "Настройки сохранены (mock)."
            self.send_text(STATE.make_status_plain())
            return

        if self.path.startswith("/digitMove"):
            STATE.apply_settings_body(body)
            digit = float(body.get("digitCount", 0.0))
            ok, text = STATE.start_digit_move(digit)
            if not ok:
                self.send_text(text, 409)
            else:
                self.send_text(text)
            return

        if self.path.startswith("/rotateCounter") or self.path.startswith("/rotateDisk"):
            STATE.apply_settings_body(body)
            self.send_text("В mock-сервере используйте +/- (digitMove).", 501)
            return

        if self.path.startswith("/counterValue"):
            if "value" not in body:
                self.send_text('Ожидается JSON с полем "value" (целое число).', 400)
                return
            try:
                v = int(body["value"])
            except (TypeError, ValueError):
                self.send_text("Поле value должно быть целым числом.", 400)
                return
            with STATE._lock:
                STATE.counter_value = v
                STATE.data_source_line = f"Mock: notifyCounterValue({v})"
                STATE.status_text = "Значение обновлено (mock)."
            self.send_text(STATE.make_status_plain())
            return

        self.send_error(404)


def main() -> None:
    if not os.path.isdir(DATA_DIR):
        raise SystemExit(f"Нет папки data: {DATA_DIR}")
    httpd = HTTPServer((HOST, PORT), Handler)
    print(f"Mock UI: http://127.0.0.1:{PORT}/")
    print(f"С телефона (та же Wi-Fi): http://<IP_этого_ПК>:{PORT}/")
    print("Останов: Ctrl+C")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nОстанов.")


if __name__ == "__main__":
    main()
