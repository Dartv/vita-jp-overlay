#!/usr/bin/env python3
"""Rebuilds config.ini from the template, keeping the values of an existing
file and applying key=value overrides.

    migrate_config.py OLD TEMPLATE OUT [key=value ...]

OLD may be missing (first install). Renamed settings are carried over, removed
ones are dropped. Prints what changed (API keys only as set/empty). Exit
status 0 = OUT should be uploaded, 1 = OLD is already identical.
"""
import os
import re
import sys

RENAMED = {"api_key": "jpdb_api_key"}
REMOVED = {"frequency_filter", "font_size", "hw_jpeg"}


def parse(text):
    vals = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in ";#" or "=" not in line:
            continue
        k, v = line.split("=", 1)
        k = k.strip().lower()
        if not k.startswith("anki_"):  # anki_ values may contain ; and #, as in core/config.c
            v = re.split(r"\s[;#]", v, maxsplit=1)[0]  # inline comment
        vals[k] = v.strip()
    return vals


def show(k, v):
    if k.endswith("api_key"):
        return "set" if v else "empty"
    return v or "empty"


def main():
    old_path, tmpl_path, out_path, *sets = sys.argv[1:]
    old_text = open(old_path, encoding="utf-8-sig").read() if os.path.exists(old_path) else None
    old = parse(old_text) if old_text is not None else None

    vals = {}
    for k, v in (old or {}).items():
        k = RENAMED.get(k, k)
        if k not in REMOVED:
            vals[k] = v
    explicit = set()
    for s in sets:
        k, _, v = s.partition("=")
        vals[k.strip().lower()] = v.strip()
        explicit.add(k.strip().lower())

    out, used = [], set()
    for line in open(tmpl_path, encoding="utf-8").read().splitlines():
        m = re.match(r"^(\w+)\s*=", line)
        if m and m.group(1) in vals:
            line = ("%s = %s" % (m.group(1), vals[m.group(1)])).rstrip()
            used.add(m.group(1))
        out.append(line)
    # Settings outside the template (e.g. debugging switches) survive only
    # while they are set explicitly with --set or already present.
    extra = [k for k in vals if k not in used and (k in explicit or k in (old or {}))]
    if extra:
        out.append("")
        out.extend(("%s = %s" % (k, vals[k])).rstrip() for k in extra)
        used.update(extra)
    text = "\n".join(out) + "\n"
    open(out_path, "w", encoding="utf-8").write(text)

    new = parse(text)
    for k in sorted(set(old or {}) | set(new)):
        if old is not None and k in old and k not in new:
            print("config.ini: %s removed" % k)
        elif old is None or old.get(k) != new.get(k):
            print("config.ini: %s = %s" % (k, show(k, new.get(k, ""))))
    for k in sorted(set(vals) - used):
        print("config.ini: unknown setting %s dropped" % k)
    return 0 if old_text is None or old_text != text else 1


if __name__ == "__main__":
    sys.exit(main())
