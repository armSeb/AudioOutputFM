# Simulations

Numerical models of the digital signal path, used while designing the stereo and RDS code.
They port the C algorithms line by line to numpy, so a change in `src/AudioOutputFM.cpp`
(filter design, phase convention, RDS framing) can be checked here before flashing.

| Script | What it checks |
|---|---|
| `simstereo.py` | Resampler response, multiplex spectrum (images, spurs), separation with an ideal coherent decoder |
| `simrds.py` | RDS character set conversion, group/CRC framing, biphase modulation, decoded back by an independent demodulator |

Requirements: Python 3, numpy, scipy. Each run takes up to a few minutes (pure Python loops).

Reference results (Q15 taps, 44.1 kHz input): images of the source rate around −80 dB, crosstalk at 1 kHz −110 dB,
RDS groups all decoded with PS and RadioText (accents included) intact.
They do not model the APLL or the receiver: the −22° phase trim compensates a shift measured on hardware.
