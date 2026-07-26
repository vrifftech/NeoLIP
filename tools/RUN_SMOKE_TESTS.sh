#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ $# -gt 0 ]]; then
  ROOT="$1"
else
  if [[ -d "$SCRIPT_DIR/bin" && -d "$SCRIPT_DIR/models" ]]; then
    ROOT="$SCRIPT_DIR"
  else
    ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
  fi
fi

BACKEND="$ROOT/bin/neolip_backend"
if [[ ! -x "$BACKEND" ]]; then
  echo "NeoLIP private backend not found or not executable: $BACKEND" >&2
  exit 2
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/neolip_smoke.XXXXXX")"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

"$BACKEND" check-bundle "$ROOT"
"$BACKEND" text-to-ntp "$ROOT/examples/test.txt" "$TMP_DIR/smoke.ntp" --models "$ROOT/models"
"$BACKEND" align-wav-text-lip "$ROOT/examples/test.wav" "$ROOT/examples/test.txt" "$TMP_DIR/smoke.lip" --models "$ROOT/models" --work-dir "$TMP_DIR/smoke_work"
"$BACKEND" lip-info "$TMP_DIR/smoke.lip"
