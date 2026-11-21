#!/usr/bin/env bash
# 反复拉取 GET /api/board（不带 since，返回 codec1 SNAPSHOT），直到 Content-Length 完整下载。
# 成功后立刻把快照转成 PPM/PNG；失败只记日志、退避后重试。
cd "$(dirname "$0")" || exit 1
LOG=.protocol/snap_job.log
: > "$LOG"
PROXY="${PROXY:-http://127.0.0.1:7890}"
X=""
if curl -s -m 2 -o /dev/null "$PROXY" 2>/dev/null; then X="-x $PROXY"; echo "using proxy $PROXY" >> "$LOG"; else echo "proxy unreachable, direct" >> "$LOG"; fi
echo "[$(date +%F' '%H:%M:%S)] job start" >> "$LOG"
for i in $(seq 1 120); do
  ts=$(date +%H:%M:%S)
  rm -f .protocol/snap_full.zst .protocol/snap_full.hdr
  out=$(curl -sS $X --http1.1 --max-time 900 \
        -D .protocol/snap_full.hdr -o .protocol/snap_full.zst \
        -w '%{http_code} %{size_download} %{time_total}' \
        https://pb.ika.zip/api/board 2>>"$LOG")
  rc=$?
  cl=$(grep -i '^content-length:' .protocol/snap_full.hdr 2>/dev/null | tr -d '\r' | awk '{print $2}')
  et=$(grep -i '^etag:' .protocol/snap_full.hdr 2>/dev/null | tr -d '\r' | sed 's/^[^:]*: *//')
  sz=$(stat -c %s .protocol/snap_full.zst 2>/dev/null || echo 0)
  echo "[$ts] attempt=$i rc=$rc curl=($out) size=$sz cl=$cl etag=$et" >> "$LOG"
  if [ "$rc" = 0 ] && [ -n "$cl" ] && [ "$sz" = "$cl" ]; then
      echo "[$ts] SNAPSHOT COMPLETE size=$sz etag=$et" >> "$LOG"
      V=$(grep -i '^x-board-version:' .protocol/snap_full.hdr | tr -d '\r' | tr -dc '0-9')
      DELTA="-"
      if [ -n "$V" ]; then
        curl -sS $X --max-time 120 -D .protocol/catchup.hdr -o .protocol/catchup.zst \
             "https://pb.ika.zip/api/board?since=$V" >> "$LOG" 2>&1
        cs=$(stat -c %s .protocol/catchup.zst 2>/dev/null || echo 0)
        ccl=$(grep -i '^content-length:' .protocol/catchup.hdr 2>/dev/null | tr -d '\r' | awk '{print $2}')
        echo "[$ts] catchup since=$V size=$cs cl=$ccl" >> "$LOG"
        if [ -n "$ccl" ] && [ "$cs" = "$ccl" ] && [ "$cs" != 0 ]; then DELTA=.protocol/catchup.zst; fi
      fi
      testfiles/pub_apply .protocol/snap_full.zst "$DELTA" .protocol/public_board.ppm >> "$LOG" 2>&1
      echo "[$(date +%H:%M:%S)] pub_apply rc=$?" >> "$LOG"
      ffmpeg -y -loglevel error -i .protocol/public_board.ppm .protocol/public_board.png >> "$LOG" 2>&1
      echo "[$(date +%H:%M:%S)] png rc=$? DONE" >> "$LOG"
      exit 0
  fi
  sleep 20
done
echo "[$(date +%F' '%H:%M:%S)] FAILED after 120 attempts" >> "$LOG"
exit 1
