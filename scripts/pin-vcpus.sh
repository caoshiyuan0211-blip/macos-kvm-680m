#!/bin/bash
# Pin each running vCPU thread to its own physical core (one SMT thread per core).
# Usage: scripts/pin-vcpus.sh [host cpu list, default "2 3 4 5 6 7 8 9" = both SMT threads of host cores 1-4, for the default 4c/2t; >8 vCPUs default to host cpus 0..N-1]
set -euo pipefail
N=$(echo "info cpus" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock | grep -ac thread_id)
(( N > 8 )) && DEF="$(seq -s' ' 0 $((N-1)))" || DEF="2 3 4 5 6 7 8 9"
CPUS=(${1:-$DEF})
i=0
for tid in $(echo "info cpus" | socat - UNIX-CONNECT:/tmp/hkvm-monitor.sock | grep -ao "thread_id=[0-9]*" | cut -d= -f2); do
  taskset -pc "${CPUS[$i]}" "$tid" >/dev/null && echo "vCPU$i (tid $tid) -> host cpu ${CPUS[$i]}"
  i=$((i+1))
done
