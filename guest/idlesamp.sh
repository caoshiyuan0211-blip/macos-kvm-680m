#!/bin/sh
for i in $(seq 300); do
  echo "$(date +%T) $(ioreg -c IOHIDSystem -d 4 | awk '/HIDIdleTime/{print int($NF/1000000); exit}')"
  sleep 1
done
