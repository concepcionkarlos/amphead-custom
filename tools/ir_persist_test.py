"""Regression test: a stale saved path must NEVER destroy a working IR."""
import sys, numpy as np
sys.path.insert(0,"tools")
from amp_test import make_plugin, SR
import amp_state
IRDIR="/Volumes/External/untitled folder 2/OwnHammer Essentials/48000 Hz/412 FMAN"
A=IRDIR+"/OH 412 FMAN 57.wav"; B=IRDIR+"/OH 412 FMAN 121.wav"
BOGUS_A="/Users/nobody/gone/OH 412 FMAN 57.wav"
BOGUS_B="/Users/nobody/gone/OH 412 FMAN 121.wav"

def hf(p):
    """energy 8-16k relative to 200-2k: low = a cab is in the path."""
    t=np.arange(int(SR*2.0))/SR
    x=np.zeros_like(t)
    for h,a in enumerate([1,.6,.4,.3,.2,.15,.1,.08,.05],start=1): x+=a*np.sin(2*np.pi*110*h*t)
    x*=np.exp(-t/1.2); x=(0.25*x/np.abs(x).max()).astype(np.float32)
    y=p(x.reshape(1,-1), SR, reset=True); y=y.mean(axis=0) if y.ndim>1 else y
    S=np.abs(np.fft.rfft(y*np.hanning(len(y)))); fr=np.fft.rfftfreq(len(y),1/SR)
    b=lambda a,c:20*np.log10(np.sqrt((S[(fr>=a)&(fr<c)]**2).mean())+1e-20)
    return b(8000,16000)-b(200,2000)

p=make_plugin(2, bypass_ir=False, ir_mix=1.0)
print("1. sin IR (head-only)          8-16k rel = %+.1f dB" % hf(p))
p.raw_state = amp_state.his_state_with_irs(A,B)
p.bypass_ir=False; p.ir_mix=1.0
with_ir = hf(p)
print("2. con las IR reales cargadas   8-16k rel = %+.1f dB" % with_ir)

# now hand it a state whose IR paths are dead - the old code called clearIRA()
xml = amp_state.set_ir_paths(amp_state.his_inner_xml(), A, B)
xml = xml.replace(A, BOGUS_A).replace(B, BOGUS_B)
p.raw_state = amp_state.build_raw_state(xml)
p.bypass_ir=False; p.ir_mix=1.0
after = hf(p)
print("3. tras recibir un estado con rutas MUERTAS  8-16k rel = %+.1f dB" % after)
print()
if abs(after - with_ir) < 3.0:
    print("   PASS - la IR sigue cargada. Una ruta obsoleta ya no destruye tu cab.")
else:
    print("   FAIL - la IR se perdio (%.1f -> %.1f dB): el estado muerto la borro." % (with_ir, after))
