#!/usr/bin/env bash
# 拉取一次公共增量：每一轮都重新取 meta 再立刻取 board?since，避免 since 过期被退回 1.2MB 快照。
# 输出：.protocol/live.zst（zstd frame）、.protocol/live.hdr、.protocol/changes.csv（tick,idx,w）
# w 是颜色配方：0=用历史层 l1，1=用 l2，2=原地重绘（颜色不变），其它=ARGB 颜色。
set -u
cd "$(dirname "$0")"
D=.protocol
PROXY="${PROXY:-http://127.0.0.1:7890}"
X=""
if curl -s -m 2 -o /dev/null "$PROXY" 2>/dev/null; then X="-x $PROXY"; echo "using proxy $PROXY"; else echo "proxy unreachable, direct"; fi
for r in 1 2 3 4 5 6; do
  meta=$D/meta_live.json
  if ! curl -sS $X --max-time 40 -o "$meta" https://pb.ika.zip/api/meta; then echo "meta failed (round $r)" >&2; continue; fi
  read -r P0 W H < <(python3 - "$meta" <<'PY'
import json,sys
m=json.load(open(sys.argv[1]))
print(m["public_version"], m["board"]["width"], m["board"]["height"])
PY
) || { echo "meta parse failed" >&2; continue; }
  echo "round $r: public_version=$P0 board=${W}x${H}"
  if ! curl -sS $X -D "$D/live.hdr" --max-time 90 -o "$D/live.zst" "https://pb.ika.zip/api/board?since=$P0"; then echo "board failed (round $r)" >&2; continue; fi
  cl=$(grep -i '^content-length:' "$D/live.hdr" | tr -d '\r' | awk '{print $2}')
  sz=$(stat -c %s "$D/live.zst")
  echo "  size=$sz content-length=$cl"
  if [ "$sz" = "$cl" ]; then
    grep -i '^x-board-version:' "$D/live.hdr" | tr -d '\r'
    testfiles/pub_decode "$D/live.zst" "$D/changes.csv" "$W" "$H"
    exit 0
  fi
done
echo "all rounds failed"; exit 1
