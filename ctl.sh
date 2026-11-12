#!/bin/bash
# usage: ctl.sh "command" [timeout_secs]
n0=$(wc -l < /tmp/rbo.out 2>/dev/null || echo 0)
timeout 5 sh -c "echo \"\$0\" > /tmp/rbo.cmd" "$1" || { echo "NO_SERVER"; exit 2; }
t=${2:-120}; end=$((SECONDS+t))
while [ $SECONDS -lt $end ]; do
  n1=$(wc -l < /tmp/rbo.out)
  if [ "$n1" -gt "$n0" ]; then
    last=$(tail -1 /tmp/rbo.out)
    case "$1" in
      go*|try*) case "$last" in READY*|"OK try"*|BYE*) echo "$last"; exit 0;; esac;;
      *) echo "$last"; exit 0;;
    esac
  fi
  sleep 0.5
done
echo TIMEOUT; exit 1
