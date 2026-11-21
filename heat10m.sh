#!/usr/bin/env bash
# 10 分钟公共画面活动采集：每轮取 /api/board?since=<上一轮 x-board-version>，
# 把响应体按序号存成 .zst，供离线累积热力图。直连，不走代理。
set -u
cd "$(dirname "$0")"
D=.protocol/heat10
mkdir -p "$D"
rm -f "$D"/*.zst "$D/versions.txt" "$D/hdr.txt"
X=""
echo "direct (no proxy)"
meta=$D/meta.json
curl -sS $X --max-time 40 -o "$meta" https://pb.ika.zip/api/meta || { echo "meta failed"; exit 1; }
read -r P0 W H < <(python3 - "$meta" <<'PY'
import json,sys
m=json.load(open(sys.argv[1]))
print(m["public_version"], m["board"]["width"], m["board"]["height"])
PY
) || { echo "meta parse failed"; exit 1; }
echo "$W $H" > "$D/size.txt"
echo "start public_version=$P0 board=${W}x${H}"
last=$P0
t0=$(date +%s); tend=$((t0+600)); n=0; fails=0
while [ "$(date +%s)" -lt "$tend" ]; do
  n=$((n+1)); el=$(( $(date +%s) - t0 ))
  f=$(printf '%s/%04d.zst' "$D" "$n")
  if ! curl -sS $X -D "$D/hdr.txt" --max-time 20 -o "$f" "https://pb.ika.zip/api/board?since=$last"; then
     echo "  [${el}s] #$n curl failed"; fails=$((fails+1)); rm -f "$f"; sleep 1; continue
  fi
  sz=$(stat -c %s "$f")
  if [ "$sz" = "0" ]; then echo "  [${el}s] #$n 304 v=$last"; rm -f "$f"; sleep 1; continue; fi
  newv=$(grep -i '^x-board-version:' "$D/hdr.txt" | tr -d '\r' | awk '{print $2}')
  echo "$n $last $newv $sz" >> "$D/versions.txt"
  echo "  [${el}s] #$n v=$last->$newv size=$sz"
  [ -n "$newv" ] && last=$newv
done
echo "done polls=$n fails=$fails last=$last"
