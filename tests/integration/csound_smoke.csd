<CsoundSynthesizer>
<CsOptions>
-n -d -m0
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 2
0dbfs = 1

instr 1
  a1, a2, a3, a4, a5, a6  tedcv
  k1, k2, k3, k4, k5, k6  tedcvk
  at1, at2, at3, at4      tedtrig
  kb                      tedbutton 1
  if metro(2) == 1 then
    printf "cv_a=%.4f cv_k=%.4f trig1=%.4f btn=%.1f\n", 1, k(a1), k1, k(at1), kb
  endif
  tedgate a1*0, a2*0
endin
</CsInstruments>
<CsScore>
i1 0 1.6
e
</CsScore>
</CsoundSynthesizer>
