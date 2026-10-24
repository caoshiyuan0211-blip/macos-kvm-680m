#!/bin/bash
# hackintosh-kvm guest: with WIFI=1 the host is offline, so the vmxnet3 slirp NIC (10.0.2.x, agent only)
# must not hold the default route. Move its network service to the end of the service order
# (persistent), then print the network state. Run as root by screens-after-login.sh.
ifc=$(ifconfig | awk '/^[a-z]/{i=$1; sub(":","",i)} /inet 10\.0\.2\./{print i}')
echo "slirp interface: ${ifc:-none}"
order=$(networksetup -listnetworkserviceorder)
svc=$(grep -B1 "Device: $ifc)" <<<"$order" | head -1 | sed -E 's/^\([0-9*]+\) //')
if [[ -n $ifc && -n $svc ]]; then
  others=()
  while IFS= read -r s; do s=${s#\*}; [[ $s != "$svc" ]] && others+=("$s"); done \
    < <(networksetup -listallnetworkservices | tail -n +2)
  networksetup -ordernetworkservices "${others[@]}" "$svc" && echo "service order: '$svc' moved last"
fi
networksetup -listnetworkserviceorder
ifconfig | grep -E '^[a-z]|inet |status'
netstat -rn -f inet | head -12
scutil --dns | grep -m4 nameserver
for i in $(ifconfig -l); do
  [[ $i == lo0 || $i == "$ifc" ]] && continue
  ifconfig "$i" | grep -q 'inet ' || continue
  ping -c2 -t4 -b "$i" 1.1.1.1 >/dev/null 2>&1 && r=ok || r=FAIL
  echo "ping 1.1.1.1 via $i: $r"
done
curl -s -m6 -o /dev/null -w 'curl apple.com: %{http_code}\n' http://www.apple.com || echo "curl apple.com: FAIL"
