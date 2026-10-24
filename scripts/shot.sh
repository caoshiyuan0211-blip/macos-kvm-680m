#!/bin/bash
# usage: shot.sh NAME  -> logs/shots/NAME.png + NAME-small.png
cd ~/hackintosh-kvm
python3 -c '
import socket,sys
s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(("127.0.0.1",9123));s.listen(1);s.settimeout(60)
c,_=s.accept();f=open(sys.argv[1],"wb")
while True:
  d=c.recv(65536)
  if not d:break
  f.write(d)
f.close()' "logs/shots/$1.png" &
sleep 1
scripts/guest-cmd.sh 'launchctl asuser 501 sudo -u $(stat -f %Su /dev/console) screencapture -x -D 1 /tmp/s.png; nc 10.0.2.2 9123 < /tmp/s.png' 40 >/dev/null
wait
ffmpeg -loglevel error -y -i "logs/shots/$1.png" -vf scale=1280:-1 "logs/shots/$1-small.png"
ls -l "logs/shots/$1.png"
