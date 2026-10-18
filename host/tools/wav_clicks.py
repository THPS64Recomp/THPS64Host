"""Find clicks in a WAV captured by the host's --wav option (stdlib only; no ROM data in this script).

A click is a sample-to-sample step much larger than the signal's local slope: |x[i] - x[i-1]| above
--abs (default 2000 of 32768) and above --ratio (default 6) times the median step of the 2 ms around it.
Also lists runs of exact digital zero (>= --zero-ms) that sit inside non-zero audio, and the spacing of
both (a periodic pop shows up as a steady period).

usage: python3 -I wav_clicks.py IN.wav [--from SEC] [--to SEC] [--abs N] [--ratio R] [--zero-ms MS] [--list N]
"""
import argparse
import array
import statistics
import sys
import wave


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    ap.add_argument("--abs", type=int, default=2000)
    ap.add_argument("--ratio", type=float, default=6.0)
    ap.add_argument("--zero-ms", type=float, default=2.0)
    ap.add_argument("--list", type=int, default=20)
    a = ap.parse_args()
    w = wave.open(a.wav)
    sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
    i0, i1 = int(a.t0 * sr), int(min(a.t1 * sr, n))
    w.setpos(i0)
    x = array.array("h")
    x.frombytes(w.readframes(i1 - i0))
    if sys.byteorder == "big":
        x.byteswap()
    print(f"{a.wav}: {sr} Hz, {ch} ch, {(i1 - i0) / sr:.1f} s from {a.t0:.1f} s")
    half = max(1, sr // 1000)  # 1 ms each side
    for c in range(ch):
        s = x[c::ch]
        d = [abs(s[i] - s[i - 1]) for i in range(1, len(s))]
        clicks = []
        i = 0
        while i < len(d):
            if d[i] > a.abs:
                lo, hi = max(0, i - half), min(len(d), i + half)
                med = statistics.median(d[lo:hi]) or 1
                if d[i] > a.ratio * med:
                    clicks.append((i + 1, d[i], med))
                    i += half
                    continue
            i += 1
        zeros = []
        minz = int(a.zero_ms * sr / 1000)
        i = 0
        while i < len(s):
            if s[i] == 0:
                j = i
                while j < len(s) and s[j] == 0:
                    j += 1
                if j - i >= minz and i > 0 and j < len(s):
                    zeros.append((i, j - i, s[i - 1], s[j]))
                i = j
            else:
                i += 1
        name = "LR"[c] if ch == 2 else str(c)
        print(f"  ch {name}: {len(clicks)} clicks, {len(zeros)} zero runs >= {a.zero_ms} ms inside audio")
        for k, (i, step, med) in enumerate(clicks[:a.list]):
            print(f"    click {a.t0 + i / sr:8.3f}s step {step:6d} local median step {med:6.0f}")
        for k, (i, ln, before, after) in enumerate(zeros[:a.list]):
            print(f"    zero  {a.t0 + i / sr:8.3f}s {ln / sr * 1000:6.1f} ms (sample before {before}, after {after})")
        for label, ev in (("click", [i for i, _, _ in clicks]), ("zero run", [i for i, _, _, _ in zeros])):
            if len(ev) >= 3:
                gaps = [(ev[k] - ev[k - 1]) / sr for k in range(1, len(ev))]
                print(f"    {label} spacing: median {statistics.median(gaps):.2f} s, min {min(gaps):.2f}, max {max(gaps):.2f}")


if __name__ == "__main__":
    main()
