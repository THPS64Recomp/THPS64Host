"""Sanity-check a WAV captured by the host's --wav option (no ROM data in this script).

Prints duration, level per 5 s window (RMS dBFS, peak), silent share, L/R correlation,
spectral centroid, and a tempo estimate from the onset-strength autocorrelation, which
together tell "music" apart from silence, a stuck tone or noise. Optionally writes a
spectrogram PNG (ROM-derived output: keep it local).

usage: python3 -I wav_check.py IN.wav [--png OUT.png] [--from SEC] [--to SEC]
needs numpy (any venv with numpy)
"""
import argparse
import struct
import sys
import wave
import zlib

import numpy as np


def write_png(path, img):  # img: HxW uint8 grayscale
    h, w = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--png")
    ap.add_argument("--from", dest="t0", type=float, default=0)
    ap.add_argument("--to", dest="t1", type=float, default=1e9)
    a = ap.parse_args()
    w = wave.open(a.wav)
    sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
    x = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, ch).astype(np.float32) / 32768
    x = x[int(a.t0 * sr):int(min(a.t1, n / sr) * sr)]
    mono = x.mean(axis=1)
    print(f"{a.wav}: {sr} Hz, {ch} ch, {len(x) / sr:.1f} s analysed")
    win = 5 * sr
    for i in range(0, len(mono), win):
        s = mono[i:i + win]
        rms = np.sqrt(np.mean(s ** 2)) + 1e-12
        print(f"  {a.t0 + i / sr:6.1f}s  rms {20 * np.log10(rms):6.1f} dBFS  peak {np.abs(x[i:i + win]).max():.3f}")
    frame = 1024
    nfr = len(mono) // frame
    fr = mono[:nfr * frame].reshape(nfr, frame)
    frms = np.sqrt((fr ** 2).mean(axis=1))
    silent = (frms < 10 ** (-60 / 20)).mean()
    clipped = (np.abs(x) > 0.999).mean()
    if ch == 2 and x[:, 0].std() > 0 and x[:, 1].std() > 0:
        lr = np.corrcoef(x[:, 0], x[:, 1])[0, 1]
    else:
        lr = float("nan")
    spec = np.abs(np.fft.rfft(fr * np.hanning(frame), axis=1))
    freqs = np.fft.rfftfreq(frame, 1 / sr)
    centroid = (spec * freqs).sum(axis=1) / (spec.sum(axis=1) + 1e-9)
    loud = frms > 10 ** (-50 / 20)
    flux = np.maximum(np.diff(np.log1p(spec * 100), axis=0), 0).sum(axis=1)
    flux = flux - flux.mean()
    ac = np.correlate(flux, flux, "full")[len(flux) - 1:]
    hop = frame / sr
    lags = np.arange(len(ac)) * hop
    sel = (lags >= 60 / 200) & (lags <= 60 / 60)
    best = lags[sel][np.argmax(ac[sel])] if sel.any() else 0
    strength = ac[sel].max() / ac[0] if sel.any() and ac[0] > 0 else 0
    print(f"  silent frames {silent:.1%}, clipped samples {clipped:.3%}, L/R correlation {lr:.2f}")
    if loud.any():
        print(f"  spectral centroid (non-silent) median {np.median(centroid[loud]):.0f} Hz, "
              f"IQR {np.percentile(centroid[loud], 25):.0f}-{np.percentile(centroid[loud], 75):.0f} Hz")
    print(f"  onset autocorrelation peak: {60 / best if best else 0:.1f} BPM (strength {strength:.2f})")
    if a.png:
        h = 256
        img = np.log1p(spec[:, :h].T * 50)
        img = (255 * img / img.max()).astype(np.uint8)[::-1]
        step = max(1, img.shape[1] // 1600)
        write_png(a.png, img[:, ::step])
        print(f"  spectrogram -> {a.png} (x: {hop * step:.3f} s/px, y: 0-{freqs[h - 1]:.0f} Hz)")


if __name__ == "__main__":
    sys.exit(main())
