A8M EXERCISE: every voice, every parameter on a CV.

Patch an LFO (or any CV) into input A, B or C of a channel and a handful
of related parameters move. Each channel's own A/B/C inputs are used
(Relative); amounts are half to full range at +-5 V.

  A  pitch:     Pitch (1 V/oct) and the voice's pitch controls (PTCH, TUNE,
                RAMP, RDEC, PCH2..4 ...)
  B  timbre:    Bits, Aliasing and the voice's timbre and modulation controls
  C  level/env: Level, Pan, Attack, Release, START DELAY, reverb send, delay
                send and the voice's envelope and level controls (DEC, 1ENV,
                UP/DOWN, CLEN, CVOL ...)

Presets
  1 EX TRX A      TRX-BD SD XT CP RS CB CH OH         channels 1-8
  2 EX TRX B      TRX-CY MA CL XC B2                  channels 1-5
  3 EX EFM        EFM-BD SD XT CP RS CB HH CY
  4 EX PI A       PI-BD SD MT ML MA RS RC CC
  5 EX PI B GND   PI-HH, GND-SN NS IM                 channels 1-4
  6 EX MNM A      SW-SAW SID-6581 SW-PULSE SW-ENS FM-STAT FM-PAR FM-DYN VO-6
  7 EX MNM B      GND-SIN GND-NOIS                    channels 1-2
  8 EX STEREO     1+2 EFM-BD pair, 3+4 sample pair (160-09 L/R): Stereo/Right
                  with offsets (pitch, level, controls; Bits, attack, release)
  9 EX HAAS       1+2 sample pair with a right START DELAY offset of 15 ms,
                  3+4 SW-SAW pair with 25 ms; 5 and 6 have their own START
                  DELAY (10 ms, 20 ms); C moves it
 10 EX DELAY CLK  the Delay follows a CV clock on 1A (4 pulses a beat; channel 1
                  has no pitch route so 1A stays the clock); delay sends up;
                  feedback, level and time on 8A, 8B, 8C
 11 EX REV DEP5   DEP-5 reverb (algorithm 4); decay, damp, level on 8A, 8B, 8C
 12 EX REV 70     the 70 unit, a factory program; its first three edits on 8A-8C
 13 EX REV 80     the 80 unit likewise
 14 EX GRAIN      Grain on sine, 160-09L (stereo with R) and R; A detune and
                  slide, B size, overlap, spray and shape, C scan and pan
 15 EX TLOCK      Time Lock: channel 1 follows a CV clock on 8A, 2 follows
                  channel 1, 3 a Speed of +50%, 4 its own tempo

Reverb sends of channels 1-8 sit at 25% (60% where an effect is the point)
and their CV is on C.
