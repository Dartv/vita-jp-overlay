#!/bin/sh
# Records responses for every host/samples/*.jpg into host/fixtures/<name>/:
# lens.pb, plus jpdb.json / jiten.json for each dictionary whose key is set
# ($VJO_JPDB_KEY, $VJO_JITEN_KEY; without either only Lens is recorded, and
# the tests reject a fixture that has no dictionary response).
# expected.txt comes from vjo-cli --replay with the dictionary the tests
# replay (jiten.json, else jpdb.json, else jiten; see vjo_replay_config).
# Review expected.txt before committing.
set -e
cd "$(dirname "$0")/.."
CLI=build/host/vjo-cli
[ -x "$CLI" ] || { cmake -S . -B build/host >/dev/null && cmake --build build/host >/dev/null; }
for img in host/samples/*.jpg; do
  name=$(basename "$img" .jpg)
  dir=host/fixtures/$name
  mkdir -p "$dir"
  rm -f "$dir/lens.pb" "$dir/jpdb.json" "$dir/jiten.json"
  recorded=
  # Every run records Lens again; jiten runs last, so lens.pb matches the
  # dictionary response that expected.txt is made from.
  for dict in jpdb jiten; do
    case $dict in
      jpdb) key=$VJO_JPDB_KEY ;;
      jiten) key=$VJO_JITEN_KEY ;;
    esac
    [ -n "$key" ] || continue
    "$CLI" "$img" --dict "$dict" --record "$dir" >/dev/null 2>&1 || true
    recorded=1
    # An error body (e.g. a rejected key) is not a fixture.
    if [ -f "$dir/$dict.json" ] && ! grep -q '"vocabulary"' "$dir/$dict.json"; then
      echo "$name: $dict returned an error, not kept"
      rm "$dir/$dict.json"
    fi
  done
  [ -n "$recorded" ] || "$CLI" "$img" --record "$dir" >/dev/null 2>&1 || true
  [ -f "$dir/lens.pb" ] || { echo "$name: no Lens response recorded"; continue; }
  replay=jiten
  if [ ! -f "$dir/jiten.json" ] && [ -f "$dir/jpdb.json" ]; then replay=jpdb; fi
  # Exits 1 when the overlay shows an error (no dictionary response).
  "$CLI" --replay "$dir" --dict "$replay" > "$dir/expected.txt" || true
  echo "$name: $(wc -c < "$dir/lens.pb") B lens$(for d in jpdb jiten; do
    [ -f "$dir/$d.json" ] && printf ', %s B %s' "$(wc -c < "$dir/$d.json")" "$d"; done), expected.txt with $replay"
done
