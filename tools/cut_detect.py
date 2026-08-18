#!/usr/bin/env python
"""
Score how an amp handles real playing dynamics, and find genuine cuts.

FIRST VERSION OF THIS TOOL WAS WRONG and the mistake is worth keeping written
down. It scored `gain = output envelope - input envelope` and called every dip
in that a "cut". But this amp is so compressed that its output barely moves, so
`gain` is just the input envelope inverted: every loud note became a "cut". It
reported 125 cuts in 50 s of playing, and essentially all of them were the
player hitting the strings harder. A metric that restates the input is worse
than no metric.

What it measures now:

  WALL     what the amp does to dynamics: input range vs output range over real
           playing, and how much of the time the output sits pinned near one
           level. This is the honest headline for a heavily compressed amp.

  CUTS     a genuine gate signature, which is NOT the same as compression:
           the output falling hard while the input does NOT fall. Compression
           can never do that - it only ever reduces gain when the input RISES.
           Anything found here is a real artefact.

    ./.venv/bin/python tools/cut_detect.py out.wav out_IN.wav --label "LEAD + IR"

Window sizes matter. An envelope window shorter than the period of the note
tracks the waveform, not the envelope: at low E the period is 12.2 ms, so a 2 ms
window reports nonsense. That mistake was also made in this project. Default 25 ms.
"""
import argparse

import numpy as np
import soundfile as sf

SR = 48000


def env_db(x, win_ms=25.0, hop_ms=5.0):
    w = int(SR * win_ms / 1000)
    h = int(SR * hop_ms / 1000)
    n = max(0, (len(x) - w) // h + 1)
    sq = x.astype(np.float64) ** 2
    e = np.array([np.sqrt(sq[i * h:i * h + w].mean() + 1e-20) for i in range(n)])
    return 20 * np.log10(e + 1e-12), h / SR


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outfile")
    ap.add_argument("infile")
    ap.add_argument("--win", type=float, default=25.0)
    ap.add_argument("--hop", type=float, default=5.0)
    ap.add_argument("--floor", type=float, default=45.0)
    ap.add_argument("--drop", type=float, default=6.0,
                    help="dB the output must fall to count as a cut")
    ap.add_argument("--window-ms", type=float, default=60.0,
                    help="how fast that fall must happen")
    ap.add_argument("--label", default="")
    args = ap.parse_args()

    yo = sf.read(args.outfile, dtype="float32", always_2d=True)[0].mean(axis=1)
    yi = sf.read(args.infile, dtype="float32", always_2d=True)[0].mean(axis=1)
    n = min(len(yo), len(yi))
    eo, hop = env_db(yo[:n], args.win, args.hop)
    ei, _ = env_db(yi[:n], args.win, args.hop)
    m = min(len(eo), len(ei))
    eo, ei = eo[:m], ei[:m]
    act = ei > (ei.max() - args.floor)
    if act.sum() < 10:
        print("not enough signal to score")
        return

    tag = f"[{args.label}] " if args.label else ""
    print(f"\n{tag}{args.outfile.split('/')[-1]}")
    print(f"  {m*hop:.1f} s, {act.sum()*hop:.1f} s of playing above the floor")

    # ---- WALL ----
    pi = np.percentile(ei[act], [5, 95])
    po = np.percentile(eo[act], [5, 95])
    ri, ro = pi[1] - pi[0], po[1] - po[0]
    med = np.median(eo[act])
    w3 = 100 * np.mean(np.abs(eo[act] - med) < 3)
    w6 = 100 * np.mean(np.abs(eo[act] - med) < 6)
    print(f"  WALL  input range {ri:5.1f} dB  ->  output range {ro:5.1f} dB"
          f"   (ratio {ro/max(ri,1e-9):.3f})")
    print(f"        output within 3 dB of its median {w3:.0f}% of the time, "
          f"within 6 dB {w6:.0f}%")

    # ---- CUTS: output falls while input does not ----
    k = max(1, int(args.window_ms / 1000 / hop))
    do = eo[k:] - eo[:-k]          # change in output over the window
    di = ei[k:] - ei[:-k]          # change in input over the same window
    a = act[k:]
    cut = a & (do < -args.drop) & (di > -2.0)
    events, i = [], 0
    while i < len(cut):
        if cut[i]:
            j = i
            while j < len(cut) and cut[j]:
                j += 1
            worst = int(np.argmin(do[i:j])) + i
            events.append(((i + k) * hop, float(do[worst]), float(di[worst])))
            i = j
        else:
            i += 1
    rate = len(events) / max(m * hop, 1e-9) * 60
    print(f"  CUTS  output falling >{args.drop:.0f} dB in {args.window_ms:.0f} ms "
          f"while the input holds: {len(events)}  ({rate:.1f}/min)")
    for t, d, s in sorted(events, key=lambda e: e[1])[:6]:
        print(f"          {t:6.2f}s   out {d:+.1f} dB   while in {s:+.1f} dB")
    if not events:
        print("          none - no gate signature in this render")


if __name__ == "__main__":
    main()
