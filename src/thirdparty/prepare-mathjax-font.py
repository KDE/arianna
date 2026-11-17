#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Arianna contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract MathJax's exact zero font and use a same-origin URL in browser bundles.

Run after replacing the two bundles with an official MathJax package. Only the
font source changes; glyphs, SVG output and the original Node fallback remain.
"""
import ast
import base64
from pathlib import Path

root = Path(__file__).resolve().parent / "mathjax"
marker = "/* Arianna local zero font */"
font_path = root / "fonts/mjx-zero.otf"
for name in ("tex-svg.js", "tex-mml-svg.js"):
    path = root / name
    source = path.read_text()
    if marker in source:
        if not font_path.is_file():
            raise RuntimeError("Missing extracted MathJax zero font")
        continue
    start = source.index('["url(data:application/x-font-woff;charset=utf-8;base64,')
    end = source.index('].join("")', start) + len('].join("")')
    expression = source[start:end]
    url = "".join(ast.literal_eval(expression.removesuffix('.join("")')))
    font = base64.b64decode(url.split(",", 1)[1].split(")", 1)[0], validate=True)
    if font_path.exists() and font_path.read_bytes() != font:
        raise RuntimeError("MathJax bundles contain different zero fonts")
    font_path.parent.mkdir(parents=True, exist_ok=True)
    font_path.write_bytes(font)
    replacement = marker + '(typeof document!=="undefined"&&document.currentScript?.src?'
    replacement += '`url("${new URL("fonts/mjx-zero.otf",document.currentScript.src).href}") format("opentype")`:'
    replacement += expression + ')'
    path.write_text(source[:start] + replacement + source[end:])
