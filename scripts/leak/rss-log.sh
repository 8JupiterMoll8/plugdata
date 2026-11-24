#!/usr/bin/env bash
# Host leak 2026-11-24: log the RSS of ONE plugdata (read-only, /proc only) and give a verdict.
#
#   rss-log.sh <pid> <out.tsv> [minutes=10] [interval_s=5]
#
# Columns: t_s rss_mb swap_mb main_cpu_pct render_cpu_pct threads virt_mb max_anon_mb big_anon (the 3 most common
# anonymous mappings >= 1 MB as size_kb x count: a pixel buffer that piles up shows as one size repeating; window
# 1920x1080 at scale 2 = 33177600 B = 32400 kB). virt_mb and max_anon_mb (the largest single anonymous mapping) tell
# the two leak shapes apart: ONE mapping that keeps growing with virt far above rss + swap is a single container in a
# loop (the 2026-11-24 kills: virt 46 GB, rss + swap 19 GB, main thread 100 %); many equal mappings with virt close
# to rss + swap are fully written buffers such as pixel copies. Guard: above GUARD_MB (default 3000) the process is killed,
# but ONLY when its HOME is not /home/alphi (a scratch instance); the owner's app is never signalled.
# Verdict after the run: growth from minute 2 to the end; PASS when within +-20 MB (PASS_MB).
set -euo pipefail
pid="$1"; out="$2"; minutes="${3:-10}"; every="${4:-5}"
guard_mb="${GUARD_MB:-3000}"; pass_mb="${PASS_MB:-20}"
[ -r "/proc/$pid/status" ] || { echo "rss-log: no process $pid" >&2; exit 2; }
[ "$(cat /proc/$pid/comm)" = "plugdata" ] || { echo "rss-log: $pid is not plugdata" >&2; exit 2; }
home="$(tr '\0' '\n' < /proc/$pid/environ 2>/dev/null | sed -n 's/^HOME=//p')"
scratch=0; [ -n "$home" ] && [ "$home" != "/home/alphi" ] && scratch=1
hz="$(getconf CLK_TCK)"
ticks() { awk '{print $14+$15}' "/proc/$pid/task/$1/stat" 2>/dev/null || echo 0; }
render_tid() { for t in /proc/$pid/task/*; do [ "$(cat $t/comm)" = "OpenGL Renderer" ] && basename $t && return; done; echo "$pid"; }
rtid="$(render_tid)"
printf 't_s\trss_mb\tswap_mb\tmain_cpu\trender_cpu\tthreads\tvirt_mb\tmax_anon_mb\tbig_anon\n' > "$out"
start=$(date +%s); prevM=$(ticks $pid); prevR=$(ticks $rtid); prevT=$start
end=$((start + ${DURATION_S:-$((minutes * 60))}))
while [ "$(date +%s)" -lt "$end" ] && [ -r "/proc/$pid/status" ]; do
  sleep "$every"
  now=$(date +%s); dt=$((now - prevT)); [ $dt -lt 1 ] && dt=1
  rss=$(awk '/^VmRSS/{printf "%d", $2/1024}' /proc/$pid/status)
  swp=$(awk '/^VmSwap/{printf "%d", $2/1024}' /proc/$pid/status)
  thr=$(awk '/^Threads/{print $2}' /proc/$pid/status)
  virt=$(awk '/^VmSize/{printf "%d", $2/1024}' /proc/$pid/status)
  maxanon=$(awk '$6=="" { split($1,a,"-"); kb=(strtonum("0x" a[2]) - strtonum("0x" a[1])) / 1024; if (kb > m) m = kb } END { printf "%d", m/1024 }' /proc/$pid/maps 2>/dev/null || echo 0)
  m=$(ticks $pid); r=$(ticks $rtid)
  mcpu=$(( (m - prevM) * 100 / (hz * dt) )); rcpu=$(( (r - prevR) * 100 / (hz * dt) ))
  prevM=$m; prevR=$r; prevT=$now
  big=$(awk '$6=="" { split($1,a,"-"); kb=(strtonum("0x" a[2]) - strtonum("0x" a[1])) / 1024; if (kb >= 1024) c[int(kb)]++ }
             END { for (k in c) print c[k], k }' /proc/$pid/maps 2>/dev/null | sort -rn | head -3 | awk '{printf "%skBx%s ", $2, $1}')
  printf '%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\n' $((now - start)) "$rss" "$swp" "$mcpu" "$rcpu" "$thr" "${virt:-0}" "${maxanon:-0}" "$big" >> "$out"
  if [ "$rss" -gt "$guard_mb" ]; then
    if [ $scratch = 1 ]; then
      echo "rss-log: RSS ${rss} MB > ${guard_mb} MB: killing scratch plugdata $pid (HOME=$home)" | tee -a "$out" >&2
      kill -9 "$pid" || true
    else
      echo "rss-log: RSS ${rss} MB > ${guard_mb} MB on the OWNER's plugdata ($pid): not touching it, tell the owner" | tee -a "$out" >&2
    fi
    break
  fi
done
awk -v pass="$pass_mb" 'NR > 1 && $1 >= 120 && first == "" { first = $2 } NR > 1 { last = $2; t = $1 }
  END { if (first == "") { print "verdict: too short (needs > 2 min)"; exit }
        g = last - first; printf "verdict: RSS %d -> %d MB from 2 min to %d s (growth %+d MB): %s\n", first, last, t, g, (g <= pass && g >= -pass) ? "PASS" : "FAIL" }' "$out" | tee -a "$out"
