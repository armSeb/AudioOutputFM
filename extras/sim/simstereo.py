"""Stereo multiplex simulation for AudioOutputFM.

Ports the C resampler (256 phases x 32 Kaiser-windowed sinc taps, Q15) and the multiplex
builder (pilot NCO, 38 kHz = 2x pilot phase + trim) to numpy, feeds a left-only 1 kHz tone,
and prints the filter response, the multiplex spectrum (images, spurs) and the separation an
ideal coherent decoder gets. It models the digital part only: the APLL and the receiver are
not simulated (the -22 degree trim compensates a phase shift measured on real hardware).

    python3 simstereo.py      (numpy + scipy)
"""
import numpy as np
from scipy import signal
T,P=32,256; fsin=44100.0; fsout=40e6/300
# --- design (same as C) ---
N=T*P; fc=min(17000,0.42*fsin); wc=fc/(fsin*P); beta=5.65; half=N/2
def proto(i):
    t=i-half; s=np.where(t==0,2*wc,np.sin(2*np.pi*wc*t)/(np.pi*np.where(t==0,1,t)))
    r=t/half; w=np.where(np.abs(r)<=1,np.i0(beta*np.sqrt(np.clip(1-r*r,0,None)))/np.i0(beta),0)
    return s*w
taps=np.zeros((P+1,T))
for p in range(P+1):
    j=np.arange(T); taps[p]=proto((T-1-j)*P+p)
g=P/sum(taps[p].sum() for p in range(P)); taps*=g
taps=np.round(taps*32768)/32768          # Q15, as stored by the C code
# prototype response
h=proto(np.arange(N+1))*g/P
wf,H=signal.freqz(h,worN=1<<16,fs=fsin*P)
for f in (10e3,14.5e3,15e3,17e3,19e3,23e3,29.1e3,40e3):
    print(f"proto {f/1e3:5.1f} kHz: {20*np.log10(abs(H[np.argmin(abs(wf-f))])+1e-12):7.1f} dB")
# --- run ---
sinlut=np.sin(2*np.pi*np.arange(1024)/1024)
def run(Lsig,Rsig,phdeg=0.0,kd=1.0,pil=0.09):
    aa=1-pil-0.01; step=fsin/fsout; phinc=int(round(19000/fsout*2**32)); phoff=int(round(phdeg/360*2**32))&0xffffffff
    hl=np.zeros(2*T); hr=np.zeros(2*T); hw=0; frac=0.0; ph=0; out=[]; phs=[]
    for l,r in zip(Lsig,Rsig):
        hl[hw]=hl[hw+T]=l; hr[hw]=hr[hw+T]=r; wl=hl[hw+1:hw+1+T].copy(); wr=hr[hw+1:hw+1+T].copy(); hw=(hw+1)%T
        while frac<1.0:
            p=int(frac*P+0.5); c=taps[p]
            L=wl@c; R=wr@c; mid=.5*(L+R); side=.5*(L-R)
            pilv=sinlut[ph>>22]; sub=sinlut[(((ph<<1)+phoff)&0xffffffff)>>22]
            out.append(aa*(mid+kd*side*sub)+pil*pilv); phs.append(ph); ph=(ph+phinc)&0xffffffff; frac+=step
        frac-=1.0
    return np.array(out), np.array(phs,dtype=np.float64)
dur=0.25; n=int(fsin*dur); t=np.arange(n)/fsin
L=0.5*np.sin(2*np.pi*1000*t); R=np.zeros(n)
mpx,phs=run(L,R)
print("outputs/input", len(mpx)/n, "expected", fsout/fsin)
# spectrum
x=mpx[2000:]*np.blackman(len(mpx)-2000); X=20*np.log10(abs(np.fft.rfft(x))+1e-12); X-=X.max(); fr=np.fft.rfftfreq(len(x),1/fsout)
def lvl(f): i=np.argmin(abs(fr-f)); return X[max(0,i-3):i+4].max()
for f in (1000,19000,37000,38000,39000,18000,20000,43100,45100,57000):
    print(f"MPX {f/1e3:5.1f} kHz: {lvl(f):7.1f} dB")
mask=(fr>200)&(fr<60000)
for f in (1000,19000,37000,39000): mask &= abs(fr-f)>300
print("worst spur 0-60 kHz:", round(X[mask].max(),1), "dB at", round(fr[mask][X[mask].argmax()]), "Hz")
# ideal coherent stereo decoder (38 kHz = 2x pilot phase, no trim)
phi=phs/2**32*2*np.pi
b,a=signal.butter(6,15000,fs=fsout)
mid=signal.filtfilt(b,a,mpx); side=signal.filtfilt(b,a,2*mpx*np.sin(2*phi))
Ld=mid+side; Rd=mid-side
sl=slice(4000,-4000)
def tone(x):
    x=x[sl]; return np.sqrt(np.mean(x**2))
print(f"separation (R/L for L-only tone): {20*np.log10(tone(Rd)/tone(Ld)):.1f} dB")
def at1k(x):
    x=x[sl]; n=np.arange(len(x)); return abs(np.sum(x*np.exp(-2j*np.pi*1000*n/fsout)))
print(f"separation at 1 kHz only: {20*np.log10(at1k(Rd)/at1k(Ld)):.1f} dB")
for ph in (-22.0,):
    mpx2,phs2=run(L,R,phdeg=ph)
    phi2=phs2/2**32*2*np.pi
    mid2=signal.filtfilt(b,a,mpx2); side2=signal.filtfilt(b,a,2*mpx2*np.sin(2*phi2))
    print(f"with h={ph}: ideal decoder separation {20*np.log10(at1k(mid2-side2)/at1k(mid2+side2)):.1f} dB (expected ~ -20 dB, sanity check)")
