#!/usr/bin/env python3
"""Локальный просмотр веб-интерфейса: статика из data/ + мок /api/state, /settings, /digitMove."""

from __future__ import annotations

import json
import threading
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path

ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data"
PORT = 8765

mock: dict = {
    "busy": False,
    "counterEnabled": True,
    "diskEnabled": False,
    "motorCounterSpeedRPM": 16,
    "motorDiskSpeedRPM": 5,
    "stepsPerDigit": 6600,
    "stepsPerRevolution": 2048,
    "subscribers": 60,
    "youtubeLine": "YouTube: локальный мок",
    "statusLine": "Локальный сервер. Моторы не крутятся.",
    "lastDigits": 0,
    "lastSteps": 0,
    "activeSteps": 0,
    "activeDigits": 0,
    "lastYoutubeTime": "20:01:58",
}
mock_lock = threading.Lock()


def compute_steps(digit_count: float, steps_per_digit: int) -> int:
    mag = abs(digit_count)
    sgn = 1 if digit_count >= 0 else -1
    if abs(mag - 0.1) < 0.001:
        return 150 * sgn
    if abs(mag - 0.25) < 0.001:
        return 500 * sgn
    return int(round(steps_per_digit * digit_count))


def move_duration_sec(steps: int, rpm: int, spr: int) -> float:
    rpm = max(1, rpm)
    sps = (rpm / 60.0) * spr
    return max(0.4, abs(steps) / sps)


def finish_move_after(steps: int, digit_count: float, delay: float) -> None:
    time.sleep(delay)
    with mock_lock:
        mock["busy"] = False
        mock["activeSteps"] = 0
        mock["activeDigits"] = 0
        mock["lastSteps"] = steps
        mock["lastDigits"] = digit_count
        mock["statusLine"] = f"Мок: готово, шагов {steps}"


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(DATA), **kwargs)

    def log_message(self, fmt, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {args[0] if args else fmt}")

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            self.path = "/index.html"
            return super().do_GET()
        if self.path == "/api/state":
            self.send_json(mock)
            return
        return super().do_GET()

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode("utf-8", errors="replace")
        if self.path == "/settings":
            try:
                body = json.loads(raw) if raw else {}
            except json.JSONDecodeError:
                self.send_error(400, "bad json")
                return
            with mock_lock:
                mock["stepsPerRevolution"] = int(body.get("stepsPerRevolution", mock["stepsPerRevolution"]))
                mock["motorCounterSpeedRPM"] = int(body.get("motorCounterSpeedRPM", mock["motorCounterSpeedRPM"]))
                mock["motorDiskSpeedRPM"] = int(body.get("motorDiskSpeedRPM", mock["motorDiskSpeedRPM"]))
                mock["stepsPerDigit"] = int(body.get("stepsPerDigit", mock["stepsPerDigit"]))
                mock["counterEnabled"] = bool(body.get("counter", mock["counterEnabled"]))
                mock["diskEnabled"] = bool(body.get("disk", mock["diskEnabled"]))
                mock["statusLine"] = "Настройки (мок) сохранены."
            self.send_response(200)
            self.end_headers()
            return
        if self.path == "/digitMove":
            try:
                body = json.loads(raw) if raw else {}
            except json.JSONDecodeError:
                self.send_error(400, "bad json")
                return
            with mock_lock:
                if mock["busy"]:
                    self.send_error(409, "busy")
                    return
                if not mock["counterEnabled"]:
                    self.send_error(400, "counter off")
                    return
                mock["motorCounterSpeedRPM"] = int(body.get("motorCounterSpeedRPM", mock["motorCounterSpeedRPM"]))
                mock["stepsPerDigit"] = int(body.get("stepsPerDigit", mock["stepsPerDigit"]))
                mock["stepsPerRevolution"] = int(body.get("stepsPerRevolution", mock["stepsPerRevolution"]))
                digit_count = float(body.get("digitCount", 0))
                steps = compute_steps(digit_count, mock["stepsPerDigit"])
                if steps == 0:
                    self.send_error(400, "zero steps")
                    return
                delay = move_duration_sec(steps, mock["motorCounterSpeedRPM"], mock["stepsPerRevolution"])
                mock["busy"] = True
                mock["activeSteps"] = steps
                mock["activeDigits"] = digit_count
                mock["statusLine"] = f"Мок: крутим {digit_count} цифр…"
            threading.Thread(
                target=finish_move_after,
                args=(steps, digit_count, delay),
                daemon=True,
            ).start()
            self.send_response(200)
            self.end_headers()
            return
        self.send_error(404)

    def send_json(self, obj: dict):
        with mock_lock:
            data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    if not DATA.is_dir():
        raise SystemExit(f"Нет папки {DATA}")
    httpd = HTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Na PK: http://127.0.0.1:{PORT}/")
    print(f"Telefon (ta zhe Wi-Fi set): http://<IP_kompyutera>:{PORT}/")
    print("Ctrl+C - stop.")
    httpd.serve_forever()


if __name__ == "__main__":
    main()
