#!/usr/bin/env python3
"""
Создаёт secrets.h для скетча Arduino из файла .env в этой же папке.
Запуск из каталога со скетчем: python gen_secrets_from_env.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ENV_PATH = ROOT / ".env"
OUT_PATH = ROOT / "secrets.h"

REQUIRED_KEYS = ("WIFI_SSID", "WIFI_PASSWORD")


def c_string_literal(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def parse_env(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$", line)
        if not m:
            continue
        key, raw = m.group(1), m.group(2)
        val = raw.strip()
        if (len(val) >= 2 and val[0] == val[-1] and val[0] in "\"'"):
            val = val[1:-1]
        out[key] = val
    return out


def main() -> int:
    env = parse_env(ENV_PATH)
    missing = [k for k in REQUIRED_KEYS if not env.get(k)]
    if missing:
        print(
            f"Нет файла .env или не заданы ключи: {', '.join(missing)}.\n"
            f"Скопируйте .env.example в .env и заполните, либо создайте secrets.h из secrets.example.h",
            file=sys.stderr,
        )
        return 1

    body = "\n".join(
        [
            "#pragma once",
            "// Сгенерировано gen_secrets_from_env.py из .env — не коммитьте в git.",
            "",
            f"#define WIFI_SSID {c_string_literal(env['WIFI_SSID'])}",
            f"#define WIFI_PASSWORD {c_string_literal(env['WIFI_PASSWORD'])}",
            "",
        ]
    )
    OUT_PATH.write_text(body, encoding="utf-8")
    print(f"Записан {OUT_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
