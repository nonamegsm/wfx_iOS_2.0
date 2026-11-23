#!/usr/bin/env python3
"""Checks src/lang.c: every text must exist in all four languages and use the
same printf placeholders in the same order. A mismatch would crash the plugin
when the message is shown, so run this after editing translations:

    python tools/check_lang.py
"""
import re
import sys
from pathlib import Path

source = Path(__file__).resolve().parent.parent / "src" / "lang.c"
text = source.read_text(encoding="utf-8")

# One entry:  [S_NAME] = { L"..." L"...", L"...", ... },
entry = re.compile(r"\[(S_[A-Z_]+)\]\s*=\s*\{(.*?)\}\s*,", re.S)
literal = re.compile(r'L"((?:[^"\\]|\\.)*)"')
placeholder = re.compile(r"%(?:[-+ 0#]*\d*(?:\.\d+)?)(hs|I64u|I64d|ls|[sdufcx%])")

errors = 0
count = 0
for name, body in entry.findall(text):
    count += 1
    # Split the four languages at top-level commas (commas inside strings do not count).
    parts, depth, current, in_string, escaped = [], 0, "", False, False
    for ch in body:
        if in_string:
            current += ch
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
        elif ch == '"':
            in_string = True
            current += ch
        elif ch == ",":
            parts.append(current)
            current = ""
        else:
            current += ch
    if current.strip():
        parts.append(current)
    texts = ["".join(literal.findall(p)) for p in parts]
    if len(texts) != 4:
        print(f"{name}: {len(texts)} languages instead of 4")
        errors += 1
        continue
    shapes = [[m for m in placeholder.findall(t) if m != "%"] for t in texts]
    if any(shape != shapes[0] for shape in shapes[1:]):
        print(f"{name}: placeholders differ: {shapes}")
        errors += 1
    if any(not t for t in texts):
        print(f"{name}: empty text")
        errors += 1

print(f"{count} texts checked, {errors} problem(s)")
sys.exit(1 if errors else 0)
