#!/usr/bin/env bash
# 连续 2 分钟轮询 /api/board?since=<上次版本>，累积逐像素变更（tick,idx,w）用于热力图。
# 每轮先不必重取 meta：since 用上一轮响应头里的 x-board-version，天然紧邻。
set -u
cd "$(dirname "$0")"
D=.protocol
X=""
echo "direct (no proxy)"

meta=$D/h2m_meta.json
curl -sS $X --max-time 40 -o "$meta" https://pb.ika.zip/api/meta || { echo "meta failed"; exit 1; }
read -r P0 W H < <(python3 - "$meta" <<'PY'
import json,sys
m=json.load(open(sys.argv[1]))
print(m["public_version"], m["board"]["width"], m["board"]["height"])
PY
) || { echo "meta parse failed"; exit 1; }
echo "start public_version=$P0 board=${W}x${H}"
printf 'tick,idx,w\n' > $D/h2m.csv
last=$P0
t0=$(date +%s); tend=$((t0+120)); n=0; gaps=0
while [ "$(date +%s)" -lt "$tend" ]; do
  n=$((n+1)); el=$(( $(date +%s) - t0 ))
  if ! curl -sS $X -D $D/h2m.hdr --max-time 30 -o $D/h2m.zst "https://pb.ika.zip/api/board?since=$last"; then
     echo "  [${el}s] poll $n curl failed"; sleep 1; continue
  fi
  sz=$(stat -c %s $D/h2m.zst)
  if [ "$sz" = "0" ]; then echo "  [${el}s] poll $n 304 v=$last"; sleep 1; continue; fi
  if ! ./testfiles/pub_decode $D/h2m.zst $D/h2m_chunk.csv "$W" "$H" > /dev/null 2>$D/h2m_err.log; then
     echo "  [${el}s] poll $n decode failed: $(head -1 $D/h2m_err.log) -> resync meta"
     gaps=$((gaps+1))
     curl -sS $X --max-time 30 -o "$meta" https://pb.ika.zip/api/meta || { sleep 2; continue; }
     last=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["public_version"])' "$meta")
     sleep 1; continue
  fi
  tail -n +2 $D/h2m_chunk.csv >> $D/h2m.csv
  cnt=$(( $(wc -l < $D/h2m_chunk.csv) - 1 ))
  newv=$(grep -i '^x-board-version:' $D/h2m.hdr | tr -d '\r' | awk '{print $2}')
  echo "  [${el}s] poll $n v=$last->$newv size=$sz entries=$cnt"
  [ -n "$newv" ] && last=$newv
  sleep 1
done
echo "done polls=$n resync=$gaps entries=$(( $(wc -l < $D/h2m.csv) - 1 )) last=$last"
