import os, pty, sys, time, select
d = sys.argv[1]
m, s = pty.openpty()
link = os.path.join(d, 'port'); 
if os.path.lexists(link): os.unlink(link)
os.symlink(os.ttyname(s), link)
log = open(os.path.join(d, 'lcd.log'), 'a', buffering=1)
inj = os.path.join(d, 'inject')
buf = b''; t0 = time.time()
while True:
    r, _, _ = select.select([m], [], [], 0.05)
    if r:
        try: buf += os.read(m, 4096)
        except OSError: time.sleep(0.05)
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            log.write('%6.2f %r\n' % (time.time() - t0, line.decode('utf8', 'replace')))
    if os.path.exists(inj):
        data = open(inj, 'rb').read(); os.unlink(inj); os.write(m, data)
