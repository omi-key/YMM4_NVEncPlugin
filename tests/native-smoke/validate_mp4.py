# Usage: python3 -I validate_mp4.py <file> <h264|hevc|av1> <width> <height> <fps> <frames> [audio 0|1]
# Needs ffmpeg/ffprobe on PATH. Checks codec/size/packet count, full decode, marker order (NvencSmoke), mvhd/tkhd/mdhd durations.
import sys, subprocess, struct, json
def boxes(b, off, end):
    while off + 8 <= end:
        sz, t = struct.unpack('>I4s', b[off:off+8]); hdr = 8
        if sz == 1: sz = struct.unpack('>Q', b[off+8:off+16])[0]; hdr = 16
        if sz == 0: sz = end - off
        yield t.decode('latin1'), off+hdr, off+sz
        off += sz
def walk(b, off, end, path, out):
    for t, s, e in boxes(b, off, end):
        p = path + '/' + t
        if t in ('moov','trak','mdia'): walk(b, s, e, p, out)
        elif t == 'mvhd':
            v = b[s]; 
            ts, d = struct.unpack('>II', b[s+12:s+20]) if v == 0 else (struct.unpack('>I', b[s+20:s+24])[0], struct.unpack('>Q', b[s+24:s+32])[0])
            out.append((p, ts, d))
        elif t == 'tkhd':
            v = b[s]; d = struct.unpack('>I', b[s+20:s+24])[0] if v == 0 else struct.unpack('>Q', b[s+28:s+36])[0]
            out.append((p, None, d))
        elif t == 'mdhd':
            v = b[s]; ts, d = struct.unpack('>II', b[s+12:s+20]) if v == 0 else (struct.unpack('>I', b[s+20:s+24])[0], struct.unpack('>Q', b[s+24:s+32])[0])
            out.append((p, ts, d))
f, codec, W, H, fps, n = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
audio = int(sys.argv[7]) if len(sys.argv) > 7 else 1
res = {}
pr = json.loads(subprocess.run(['ffprobe','-v','error','-count_packets','-show_streams','-show_format','-of','json',f],capture_output=True,text=True).stdout)
vs = [s for s in pr['streams'] if s['codec_type']=='video']; as_ = [s for s in pr['streams'] if s['codec_type']=='audio']
v = vs[0]
ok = True; msgs=[]
def chk(c, m):
    global ok
    if not c: ok = False; msgs.append(m)
chk(v['codec_name'] == {'h264':'h264','hevc':'hevc','av1':'av1'}[codec], 'codec '+v['codec_name'])
chk((v['width'], v['height']) == (W, H), 'res')
chk(int(v['nb_read_packets']) == n, 'packets %s'%v['nb_read_packets'])
vdur = float(v.get('duration', 0))
adur = float(as_[0].get('duration', 0)) if as_ else 0
if audio: chk(len(as_) == 1, 'no audio stream')
# box parse
b = open(f,'rb').read(); out = []; walk(b, 0, len(b), '', out)
res['boxes'] = out
mv = [o for o in out if o[0].endswith('mvhd')][0]
mvdur = mv[2]/mv[1]
tk = [o[2]/mv[1] for o in out if o[0].endswith('tkhd')]
md = [o[2]/o[1] for o in out if o[0].endswith('mdhd')]
exp = n/fps
chk(abs(vdur-exp) < 0.05, 'vdur %.3f vs %.3f'%(vdur, exp))
if audio and as_: chk(abs(adur-exp) < 0.1, 'adur %.3f vs %.3f'%(adur, exp))
for i,t in enumerate(tk): chk(abs(t-exp) < 0.1, 'tkhd[%d]=%.3fs vs %.3f'%(i,t,exp))
for i,t in enumerate(md): chk(abs(t-exp) < 0.1, 'mdhd[%d]=%.3fs vs %.3f'%(i,t,exp))
chk(abs(mvdur-exp) < 0.1, 'mvhd=%.3f'%mvdur)
# full decode
d = subprocess.run(['ffmpeg','-v','error','-i',f,'-f','null','-'],capture_output=True,text=True)
chk(d.returncode == 0 and not d.stderr.strip(), 'decode errors: '+d.stderr.strip()[:200])
# marker
raw = subprocess.run(['ffmpeg','-v','error','-i',f,'-an','-vf','crop=512:32:0:0,format=gray','-f','rawvideo','-'],capture_output=True).stdout
fs = 512*32; got = len(raw)//fs
chk(got == n, 'decoded frames %d'%got)
bad = []
for i in range(got):
    fr = raw[i*fs:(i+1)*fs]; val = 0
    for bit in range(16):
        px = fr[16*512 + bit*32 + 16]
        if px > 128: val |= 1<<bit
    if val != i: bad.append((i, val))
chk(not bad, 'marker mismatches %d first %s'%(len(bad), bad[:3]))
print('%s: %s | v=%.3fs a=%.3fs mvhd=%.3f tkhd=%s mdhd=%s | %s' % (f.split('/')[-1], 'PASS' if ok else 'FAIL', vdur, adur, mvdur, ['%.3f'%t for t in tk], ['%.3f'%t for t in md], '; '.join(msgs)))
sys.exit(0 if ok else 1)
