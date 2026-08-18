import sys, numpy as np
sys.path.insert(0,'tools')
from amp_test import make_plugin, render, plucked_note

def ratio(ch, **kw):
    pk=[]
    for a in (0.05,0.45):
        p=make_plugin(ch, **kw)
        pk.append(20*np.log10(np.abs(render(p, plucked_note(peak=a))).max()+1e-12))
    return (pk[1]-pk[0])/19.1, pk[1]-pk[0]

print("LEAD dynamics - isolating which stage compresses\n")
print("MASTER scales the POWER section (sagScale and pwrKeff both track it):")
for m in (0.0,0.3,0.6,1.0):
    r,d = ratio(2, master=m); print(f"   master={m:.1f}   {d:5.2f} dB out   ratio {r:4.2f}")
print("\nCHARACTER scales preamp asymmetry / even-harmonic drive:")
for c in (0.0,5.0,10.0):
    r,d = ratio(2, character=c); print(f"   char={c:4.1f}    {d:5.2f} dB out   ratio {r:4.2f}")
print("\nEverything minimised (gain 1, char 0, master 0):")
r,d = ratio(2, gain=1.0, character=0.0, master=0.0); print(f"   LEAD    {d:5.2f} dB out   ratio {r:4.2f}")
r,d = ratio(0, gain=1.0, character=0.0, master=0.0); print(f"   CLEAN   {d:5.2f} dB out   ratio {r:4.2f}")
