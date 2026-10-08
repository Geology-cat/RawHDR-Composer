#!/bin/bash
# 使用説明書のスクリーンショットを撮る（開発版のアプリの検証用の自動操作を使う）。
# 撮った PNG は fig/raw/（リポジトリには入れない）。説明書で使う JPEG・切り抜きはそこから作る。
# 撮った後は python3 docs/manual/make_figs.py で、説明書の図（縮小・切り抜き）を作る。
# ダイアログ（Adobe DNG Converter の案内・保存・開く）は screencapture -l で手で撮った。
#   docs/manual/capture.sh
set -uo pipefail
cd "$(dirname "$0")/../.."
APP=".build/dev/app/RawHDR Composer.app/Contents/MacOS/RawHDR Composer"
OUT="docs/manual/fig/raw"
COL="$(ls "$PWD"/test-photo/20260821*/*.CR2)"
LH="$(ls "$PWD"/test-photo/20240328*/*.CR2)"
shot() {  # shot 名前 環境変数...
    local name="$1"; shift
    ( env "$@" RBH_SNAPSHOT="$PWD/$OUT/$name.png" "$APP" >/dev/null 2>&1 & pid=$!
      for i in $(seq 1 240); do sleep 1; kill -0 $pid 2>/dev/null || break; done; kill $pid 2>/dev/null; wait $pid ) 2>/dev/null
    [ -f "$OUT/$name.png" ] && echo "ok $name" || echo "NG $name"
}
shot 01_empty
shot 02_loaded RBH_OPEN="$COL" RBH_NO_MERGE=1 RBH_SELECT=6
shot 03_before_merge RBH_OPEN="$COL" RBH_NO_MERGE=1 RBH_VIEW=0
shot 04_merged RBH_OPEN="$COL" RBH_VIEW=0
shot 05_overlay RBH_OPEN="$COL" RBH_VIEW=1
shot 06_sourcemap RBH_OPEN="$COL" RBH_VIEW=2
shot 07_changed RBH_OPEN="$COL" RBH_VIEW=0 RBH_CHANGE="compress=80" RBH_CHANGE_NO_MERGE=1
shot 08_diff RBH_OPEN="$COL" RBH_VIEW=5 RBH_CHANGE="compress=80"
shot 09_align RBH_OPEN="$LH" RBH_ALIGN=2 RBH_VIEW=4 RBH_SELECT=1
shot 10_lighthouse RBH_OPEN="$LH" RBH_VIEW=0
shot 11_lh_sourcemap RBH_OPEN="$LH" RBH_VIEW=2
shot 12_lh_overlay RBH_OPEN="$LH" RBH_VIEW=1
shot 18_lh_ghost RBH_OPEN="$LH" RBH_VIEW=6
shot 19_info RBH_OPEN="$COL" RBH_SCROLL_RIGHT=1
