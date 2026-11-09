#!/usr/bin/env python3
"""Black-box tests; exclusive access to loopback port 6578 required."""
import socket,subprocess,sys,time
binary=sys.argv[1]
def start(*args):
 p=subprocess.Popen([binary,*args],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 for _ in range(100):
  try:s=socket.create_connection(('127.0.0.1',6578),.2);s.settimeout(.7);return p,s
  except OSError:time.sleep(.02)
 p.terminate();raise AssertionError('simulator did not start')
def stop(p,s):
 s.close();p.terminate();p.wait(timeout=3)
def quiet(s,seconds):
 s.settimeout(seconds)
 try:assert not s.recv(1),'unexpected byte'
 except socket.timeout:pass
 s.settimeout(.7)
p,s=start('--enq-interval','.15')
try:
 for _ in range(3):assert s.recv(1)==b'\x05'
 s.sendall(b'\x04');assert s.recv(1)==b'\x05'
 s.sendall(b'\x01\xf7');quiet(s,.35)
 s.sendall(b'\x08\x02\x02');assert s.recv(2)==b'\x94\x02'
 s.sendall(bytes.fromhex('01 f4 23 06 01 18'));assert s.recv(1)==b'\x00'
 s.sendall(bytes.fromhex('01 f7 23 06 01'));assert s.recv(1)==b'\x18'
 s.close();s=socket.create_connection(('127.0.0.1',6578));s.settimeout(.7)
 assert s.recv(1)==b'\x05'
finally:stop(p,s)
p,s=start('--enq-interval','.15','--activity-only')
try:
 assert s.recv(1)==b'\x05'
 s.sendall(bytes.fromhex('01 f7 08 02 02'))
 assert s.recv(1)==b'\x05' # activity instead of fixture
finally:stop(p,s)
p,s=start('--enq-interval','0')
try:
 quiet(s,.35);s.sendall(b'\x04');assert s.recv(1)==b'\x05'
 s.sendall(bytes.fromhex('01 f7 08 02 02'));assert s.recv(2)==b'\x94\x02'
finally:stop(p,s)
for arg in ['-1','nan','inf','oops']:
 assert subprocess.run([binary,'--enq-interval',arg],capture_output=True).returncode==2
print('PASS: periodic activity, handshake, fragmented-request silence, fixtures, writes/readback, reconnect, activity-only, disabled timer, argument validation')
