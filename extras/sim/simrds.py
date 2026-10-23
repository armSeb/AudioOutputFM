"""RDS encoder simulation for AudioOutputFM.

Ports the RDS character-set conversion, group building (0A PS, 2A RadioText, CRC + offset
words) and the biphase 57 kHz modulation to numpy, then decodes the result with an
independent demodulator (syndrome-based block sync) and checks that PS and RadioText,
accents included, come back intact.

    python3 simrds.py         (numpy + scipy)
"""
import numpy as np
from scipy import signal
fsout=40e6/300
sinlut=np.sin(2*np.pi*np.arange(1024)/1024)
# ---- port of rdsConvert ----
ebu=[0xC1,0xC0,0xD0,ord('A'),0xD1,ord('A'),ord('A'),0x8B,0xC3,0xC2,0xD2,0xD3,0xC5,0xC4,0xD4,0xD5,
     ord('D'),0x8A,0xC7,0xC6,0xD6,ord('O'),0xD7,ord('x'),ord('O'),0xC9,0xC8,0xD8,0xD9,ord('Y'),ord('T'),0x8D,
     0x81,0x80,0x90,ord('a'),0x91,ord('a'),ord('a'),0x9B,0x83,0x82,0x92,0x93,0x85,0x84,0x94,0x95,
     ord('d'),0x9A,0x87,0x86,0x96,ord('o'),0x97,ord('/'),ord('o'),0x89,0x88,0x98,0x99,ord('y'),ord('t'),ord('y')]
base="AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
print("base len", len(base))
def conv(txt,mx,ascii=False):
    out=[]
    for ch in txt:
        cp=ord(ch)
        if 0x20<=cp<0x7f: c=cp
        elif 0xC0<=cp<=0xFF: c=ord(base[cp-0xC0]) if ascii else ebu[cp-0xC0]
        elif cp in (0x2018,0x2019): c=ord("'")
        elif cp in (0x201C,0x201D): c=ord('"')
        elif cp in (0x2013,0x2014): c=ord('-')
        elif cp==0x2026: c=ord('.')
        elif cp<0x20: continue
        else: c=ord('?')
        out.append(c)
        if len(out)>=mx: break
    return out
EBU_DEC={0x80:'á',0x81:'à',0x82:'é',0x83:'è',0x84:'í',0x85:'ì',0x86:'ó',0x87:'ò',0x88:'ú',0x89:'ù',0x8A:'Ñ',0x8B:'Ç',0x8D:'ß',
 0x90:'â',0x91:'ä',0x92:'ê',0x93:'ë',0x94:'î',0x95:'ï',0x96:'ô',0x97:'ö',0x98:'û',0x99:'ü',0x9A:'ñ',0x9B:'ç',
 0xC0:'Á',0xC1:'À',0xC2:'É',0xC3:'È',0xC4:'Í',0xC5:'Ì',0xC6:'Ó',0xC7:'Ò',0xC8:'Ú',0xC9:'Ù',
 0xD0:'Â',0xD1:'Ä',0xD2:'Ê',0xD3:'Ë',0xD4:'Î',0xD5:'Ï',0xD6:'Ô',0xD7:'Ö',0xD8:'Û',0xD9:'Ü'}
def dec(cs): return ''.join(EBU_DEC.get(c, chr(c) if 32<=c<127 else ('<CR>' if c==13 else '¿')) for c in cs)
title="Mylène Farmer - Désenchantée"
print("EBU :", dec(conv(title,64)))
print("ASCII:", ''.join(map(chr,conv(title,64,True))))
for t in ("À l'été, où ça?", "Ça Ï Ü ß ñ Ö", "“Hello” – world…", "日本"):
    print(f"{t!r:22} -> {dec(conv(t,64))!r}")
# ---- port of encoder ----
def crc(d):
    r=d<<10
    for i in range(25,9,-1):
        if r&(1<<i): r^=0x5B9<<(i-10)
    return r&0x3FF
class Enc:
    def __init__(s, pi=0xF201, pty=10, ps="ESP32 FM", rt=title, stereo=True):
        s.pi,s.pty,s.stereo=pi,pty,stereo; s.ps=conv(ps,8)+[32]*(8-len(conv(ps,8)))
        # As the C code: always 64 characters, space-padded, no 0x0D terminator (16 segments)
        rtc=conv(rt,64); rtc+= [32]*(64-len(rtc))
        s.segs=16; s.rt=rtc; s.ab=0; s.g=0; s.pss=0; s.rts=0
    def group(s):
        common=(s.pty&0x1f)<<5; b=[s.pi,0,0,0]
        if s.segs and (s.g&1):
            seg=s.rts; b[1]=(2<<12)|common|(0x10 if s.ab else 0)|seg
            b[2]=(s.rt[seg*4]<<8)|s.rt[seg*4+1]; b[3]=(s.rt[seg*4+2]<<8)|s.rt[seg*4+3]; s.rts=(s.rts+1)%s.segs
        else:
            seg=s.pss; di=1 if (seg==3 and s.stereo) else 0
            b[1]=common|(1<<3)|(di<<2)|seg; b[2]=0xE0CD; b[3]=(s.ps[seg*2]<<8)|s.ps[seg*2+1]; s.pss=(s.pss+1)&3
        s.g+=1; bits=[]
        for k,o in enumerate((0x0FC,0x198,0x168,0x1B4)):
            cw=crc(b[k])^o; bits+= [(b[k]>>i)&1 for i in range(15,-1,-1)] + [(cw>>i)&1 for i in range(9,-1,-1)]
        return bits
# ---- multiplex emission (RDS only + pilot, exactly as the C loop) ----
enc=Enc(); phinc=int(round(19000/fsout*2**32)); ph=0; cyc=0; diff=0; sign=1.0; bits=[]; bi=104
ar=0.04; ap=0.09; N=int(fsout*2.4); mpx=np.zeros(N); phs=np.zeros(N)
for n in range(N):
    u=((cyc<<28)|(ph>>4))&0xffffffff
    mpx[n]=ap*sinlut[ph>>22] + ar*sign*sinlut[u>>22]*sinlut[((ph*3)&0xffffffff)>>22]
    phs[n]=ph; prev=ph; ph=(ph+phinc)&0xffffffff
    if ph<prev:
        cyc+=1
        if cyc==16:
            cyc=0
            if bi>=104: bits=enc.group(); bi=0
            diff^=bits[bi]; bi+=1; sign=1.0 if diff else -1.0
# ---- independent receiver: coherent 57 kHz demod, biphase integrate & dump ----
phi=phs/2**32*2*np.pi
bb=mpx*np.sin(3*phi)*2
b,a=signal.butter(4,2400,fs=fsout); bb=signal.filtfilt(b,a,bb)
# bit clock from pilot: a bit = 16 pilot cycles; integrate first half minus second half
cycles=np.cumsum(np.r_[0,np.diff(phs)<0]); bitidx=cycles//16; halfpos=((cycles%16)*2**32+phs)/(16*2**32)
sym=[]
for k in range(1,int(bitidx.max())):
    m=bitidx==k; v=bb[m]; h=halfpos[m]
    sym.append(np.sum(v[h<0.5])-np.sum(v[h>=0.5]))
raw=[1 if x>0 else 0 for x in sym]
data=[raw[i]^raw[i-1] for i in range(1,len(raw))]          # differential decode
# block sync by syndrome
OFF={0x0FC:'A',0x198:'B',0x168:'C',0x350:"C'",0x1B4:'D'}
def blockinfo(bits26):
    v=int(''.join(map(str,bits26)),2); d=v>>10; c=v&0x3FF
    return d, OFF.get(crc(d)^c)
ps=[' ']*8; rt={}; ok=0; bad=0; pos=None
for i in range(len(data)-104):
    d,o=blockinfo(data[i:i+26])
    if o=='A':
        bl=[blockinfo(data[i+26*k:i+26*k+26]) for k in range(4)]
        if [x[1] for x in bl]==['A','B','C','D']: pos=i; break
assert pos is not None, "no sync"
i=pos
while i+104<=len(data):
    bl=[blockinfo(data[i+26*k:i+26*k+26]) for k in range(4)]
    if [x[1] for x in bl]!=['A','B','C','D']: bad+=1; i+=104; continue
    ok+=1; pi,B,C,D=[x[0] for x in bl]; gtype=B>>12
    if gtype==0: seg=B&3; ps[seg*2]=chr(D>>8); ps[seg*2+1]=chr(D&0xff); stereo_flag=(B>>2)&1 if seg==3 else None
    if gtype==2: seg=B&0xF; rt[seg]=[C>>8,C&0xff,D>>8,D&0xff]
    i+=104
print(f"groups OK {ok}, bad {bad}, PI {pi:04X}, PTY {(B>>5)&0x1f}")
print("PS:", repr(''.join(ps)))
rtc=sum((rt[k] for k in sorted(rt)),[])
print("RT:", repr(dec(rtc)))
