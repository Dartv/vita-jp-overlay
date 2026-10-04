#!/bin/bash
# Installs Vita JP Overlay over VitaShell's FTP server (VitaShell: SELECT ->
# FTP). Backs up the taiHEN configs first (locally and as config.txt.vjo-bak on
# the Vita) and adds the two plugin lines once. config.ini is created on the
# first install; later installs keep its values and bring it up to the current
# set of settings. Local copies (backups, logs, dumps) go to build/device/.
# Needs bash, curl and python3.
#
#   tools/install_ftp.sh 192.168.1.50[:1337]
#   tools/install_ftp.sh --set dictionary=jiten --set jiten_api_key=KEY 192.168.1.50
#   tools/install_ftp.sh --add-kernel-plugin NoPowerLimits.skprx 192.168.1.50
#     (also uploads another kernel plugin to ur0:tai/ and adds it under *KERNEL;
#      --uninstall leaves such plugins in place)
#   tools/install_ftp.sh --uninstall 192.168.1.50
#   tools/install_ftp.sh --purge 192.168.1.50     (uninstall, then delete the
#     plugin files and both VitaJPOverlay data folders; config.ini and
#     region.ini are copied to the local backup first)
#   tools/install_ftp.sh --status 192.168.1.50   (prints the plugin logs)
#   tools/install_ftp.sh --dumps 192.168.1.50    (downloads system crash dumps)
set -euo pipefail
cd "$(dirname "$0")/.."

UNINSTALL=0
PURGE=0
STATUS=0
DUMPS=0
SETS=()
EXTRA_K=()
while [ $# -gt 1 ]; do
  case $1 in
    --uninstall) UNINSTALL=1 ;;
    --purge) UNINSTALL=1; PURGE=1 ;;
    --dumps) DUMPS=1 ;;
    --status) STATUS=1 ;;
    --set) SETS+=("$2"); shift ;;
    --add-kernel-plugin) EXTRA_K+=("$2"); shift ;;
    *) break ;;
  esac
  shift
done
HOST=${1:?usage: tools/install_ftp.sh [--set key=value]... [--uninstall|--status|--dumps] VITA_IP[:PORT]}
[[ $HOST == *:* ]] || HOST=$HOST:1337
FTP=ftp://$HOST
B=build/vita
KLINE=ur0:tai/VitaJPOverlay_Kernel.skprx
SLINE=ur0:tai/VitaJPOverlay_Shell.suprx
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

c() { curl -s --show-error --max-time 60 "$@"; }
exists() { c -o /dev/null "$FTP/$1" 2>/dev/null; }
get() { c -o "$2" "$FTP/$1"; }
put() { c --ftp-create-dirs -T "$1" "$FTP/$2"; }

python3 -c 'import socket, sys; socket.create_connection((sys.argv[1], int(sys.argv[2])), 5).close()' \
    "${HOST%%:*}" "${HOST##*:}" 2>/dev/null || {
  echo "Cannot reach $HOST. In VitaShell press SELECT to start FTP, and keep the screen on."; exit 1; }

if [ $DUMPS = 1 ]; then
  # Crash dumps written by the system (psp2core-*.psp2dmp in ux0:data).
  mkdir -p build/device/dumps
  # VitaShell's FTP has no NLST and curl's directory handling trips it up:
  # list with a plain LIST via ftplib (the name is the last field).
  list=$(python3 - "${HOST%%:*}" "${HOST##*:}" <<'PY'
import ftplib, sys
f = ftplib.FTP()
f.connect(sys.argv[1], int(sys.argv[2]), timeout=20)
f.login()
lines = []
for path in ("/ux0:/data", "ux0:/data/", "/ux0:/data/"):
    try:
        f.cwd(path)
        f.retrlines("LIST", lines.append)
        break
    except ftplib.all_errors:
        lines = []
for l in lines:
    name = l.split()[-1] if l.split() else ""
    if name.lower().endswith(".psp2dmp"):
        print(name)
PY
)
  [ -n "$list" ] || { echo "no crash dumps in ux0:data"; exit 0; }
  for f in $list; do get "ux0:/data/$f" "build/device/dumps/$f" && echo "downloaded build/device/dumps/$f"; done
  exit 0
fi

if [ $STATUS = 1 ]; then
  mkdir -p build/device/status
  for f in kernel.prev kernel status.prev status; do
    echo "=== $f.txt"
    get "ux0:/data/VitaJPOverlay/$f.txt" "build/device/status/$f.txt" 2>/dev/null \
      && cat "build/device/status/$f.txt" || echo "(none)"
  done
  exit 0
fi

# Ensō + SD2Vita: taiHEN reads ur0:tai/config.txt at boot (ux0 is not the SD
# card yet), so ur0 is always patched. ux0:tai/config.txt, if present, is
# patched too, since taiHEN prefers it when ux0 is available at boot.
CFGS=(ur0:/tai/config.txt)
exists "ux0:/tai/config.txt" && CFGS+=(ux0:/tai/config.txt)
BK=build/device/backup/$(date +%Y%m%d-%H%M%S)
mkdir -p "$BK"
for CFG in "${CFGS[@]}"; do
  n=${CFG%%:*}
  get "$CFG" "$TMP/$n.txt" || { echo "cannot read $CFG"; exit 1; }
  cp "$TMP/$n.txt" "$BK/$n-config.txt"
done
echo "backup: $BK"

if [ $UNINSTALL = 1 ]; then
  for CFG in "${CFGS[@]}"; do
    n=${CFG%%:*}
    grep -v -F -e "$KLINE" -e "$SLINE" "$TMP/$n.txt" > "$TMP/$n.new" || true
    put "$TMP/$n.new" "$CFG"
    echo "removed the plugin lines from $CFG"
  done
  if [ $PURGE = 0 ]; then
    echo "Reboot the Vita (plugin files are left in place)."
    exit 0
  fi
  for f in config.ini region.ini; do
    get "ux0:/data/VitaJPOverlay/$f" "$BK/$f" 2>/dev/null && echo "saved $BK/$f" || true
  done
  # VitaShell's FTP takes absolute paths as /ur0:/...; RMD needs an empty
  # directory, so each data folder's files go first.
  python3 - "${HOST%%:*}" "${HOST##*:}" <<'PY'
import ftplib, sys
f = ftplib.FTP()
f.connect(sys.argv[1], int(sys.argv[2]), timeout=20)
f.login()
def rm(path):
    # VitaShell confirms DELE with a code ftplib does not expect (error_reply)
    try:
        f.delete(path)
    except ftplib.error_reply:
        pass
    except ftplib.all_errors:
        return
    print("deleted", path)
for name in ("VitaJPOverlay_Kernel.skprx", "VitaJPOverlay_Shell.suprx", "config.txt.vjo-bak"):
    rm("/ur0:/tai/" + name)
rm("/ux0:/tai/config.txt.vjo-bak")
for d in ("/ur0:/data/VitaJPOverlay", "/ux0:/data/VitaJPOverlay"):
    lines = []
    try:
        f.cwd(d)
        f.retrlines("LIST", lines.append)
    except ftplib.all_errors:
        continue
    for l in lines:
        if l.split() and not l.startswith("d"):
            rm(d + "/" + l.split()[-1])
    f.cwd("/")
    try:
        f.rmd(d)
        print("deleted", d)
    except ftplib.all_errors as e:
        print("could not delete", d, "-", e)
f.quit()
PY
  echo "Reboot the Vita."
  exit 0
fi

for f in "$B/VitaJPOverlay_Kernel.skprx" "$B/VitaJPOverlay_Shell.suprx" "$B/vitajpoverlay.rco" \
         ${EXTRA_K[@]+"${EXTRA_K[@]}"}; do
  [ -f "$f" ] || { echo "missing $f: run cmake --build build/vita first"; exit 1; }
done
EXTRA_LINES=()
for f in ${EXTRA_K[@]+"${EXTRA_K[@]}"}; do
  [ "$(head -c 3 "$f")" = "SCE" ] || { echo "$f is not a Vita plugin (no SCE header)"; exit 1; }
  EXTRA_LINES+=("ur0:tai/$(basename "$f")")
done

# Patch each config (CRLF-safe): kernel line after *KERNEL, shell after *main.
for CFG in "${CFGS[@]}"; do
  n=${CFG%%:*}
  python3 - "$TMP/$n.txt" "$TMP/$n.new" "$KLINE" "$SLINE" ${EXTRA_LINES[@]+"${EXTRA_LINES[@]}"} <<'PY'
import sys
src, dst, kline, sline, *extra = sys.argv[1:]
raw = open(src, "rb").read().decode("utf-8", "replace")
nl = "\r\n" if "\r\n" in raw else "\n"
lines = raw.splitlines()
def add(section, line):
    if any(l.strip() == line for l in lines):
        return
    for i, l in enumerate(lines):
        if l.strip() == section:
            lines.insert(i + 1, line)
            return
    lines.extend(["", section, line])
add("*KERNEL", kline)
for line in extra:
    add("*KERNEL", line)
add("*main", sline)
open(dst, "wb").write((nl.join(lines) + nl).encode())
PY
  echo "--- $CFG changes:"; diff "$TMP/$n.txt" "$TMP/$n.new" || true
  put "$TMP/$n.txt" "${CFG}.vjo-bak"
done

put "$B/VitaJPOverlay_Kernel.skprx" "ur0:/tai/VitaJPOverlay_Kernel.skprx"
put "$B/VitaJPOverlay_Shell.suprx" "ur0:/tai/VitaJPOverlay_Shell.suprx"
put "$B/vitajpoverlay.rco" "ur0:/data/VitaJPOverlay/vitajpoverlay.rco"
for f in ${EXTRA_K[@]+"${EXTRA_K[@]}"}; do
  put "$f" "ur0:/tai/$(basename "$f")"
  echo "uploaded ur0:tai/$(basename "$f")"
done

# config.ini: rebuilt from docs/config.example.ini with the values of the
# existing file (renamed/removed settings migrated), then any --set values.
# API keys are never printed.
INI=ux0:/data/VitaJPOverlay/config.ini
if exists "$INI"; then
  get "$INI" "$TMP/config.old"
  cp "$TMP/config.old" "$BK/config.ini"
fi
if python3 tools/migrate_config.py "$TMP/config.old" docs/config.example.ini "$TMP/config.ini" ${SETS[@]+"${SETS[@]}"}; then
  put "$TMP/config.ini" "$INI"
  [ -f "$TMP/config.old" ] && echo "config.ini updated (previous copy in $BK)" || echo "config.ini created"
else
  echo "config.ini is up to date"
fi
for CFG in "${CFGS[@]}"; do put "$TMP/${CFG%%:*}.new" "$CFG"; done
echo "Installed. Reboot the Vita. If it fails to boot, hold L while powering on and run: tools/install_ftp.sh --uninstall ${HOST%%:*}"
