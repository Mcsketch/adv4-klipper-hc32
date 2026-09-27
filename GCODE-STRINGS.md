# G-Code / M-Code String Inventory — AD4

Two firmware layers issue/parse G-code:

1. **SoC (Qt/C++ app `adventurer4-arm`)** — the UI + G-code *parser* (what the user
   sends, and the auto-generated moves). Extracted below.
2. **MCU (HC32F460 HD app)** — mostly motion/thermistor; only a few G-code
   strings are embedded (power-loss recovery / state display).

## SoC G-code parser (G-codes)

  G0  G1  G2  G3  G4  G5  G6  G7  G8  G9  G44  G47  G90  G91  G92  G161  G162

## SoC G-code parser (M-codes)

  M0  M1  M2  M3  M4  M5  M6  M7  M8  M9  M17  M18  M25  M31  M36  M37  M38  M39  M47  M63  M103  M104  M105  M107  M112  M113  M114  M117  M118  M119  M121  M122  M140  M144  M145  M652  M653  M654  M901

## MCU-embedded G-code strings (from HD app)

  0x0EB15  'G90'
  0x0EB19  'G28'
  0x0EB1D  'M132 X Y Z A B'
  0x0EB32  'G1 Z15 F400'
  0x0EB40  'M117$'

## Notable SoC auto-generated move strings (calibration / test patterns)

  'G1 Y50 F3000'
  'G1 X100 Y-90 F2000'
  'G1 Z'
  'G1 X-100 Y90 F2000'
  'G1 Y90 F3000'
  'G1 X0 Y0 F'
  'G1 Z0 F'
  'G1 X-'
  'G1 Y'
  'G1 X'
  'G1 Y-'
  'G1 Z-'
  'G1 E'
  'G1 E-5 F1000'
  'G1 Z10 F600'
  'G1 E-800 F2000'
  'G1 E520 F2000'
  'G1 E800 F180'
  'G1 E10 F180'
  'G1 E-1000 F4000'
  'G1 Z10 F300'
  'G1 X-110 Y95'
  'G1 X0 Y95'
  'G1 X100 Y95'
  'G1 X100 Y0'
  'G1 X0 Y0'
  'G1 X-110 Y0'
  'G1 X-110 Y-95'
  'G1 X0 Y-95'
  'G1 X100 Y-95'
  'G1 E-'
  'G1 Z10 F500'
  'G1 X-110 Y95 F3500'
  'G1 X-30 Y95 F3500'
  'G1 X70 Y95 F3500'
  'G1 X70 Y0 F3500'
  'G1 X-30 Y0 F3500'
  'G1 X-110 Y0 F3500'
  'G1 X-110 Y-95 F3500'
  'G1 X-30 Y-95 F3500'
  'G1 X70 Y-95 F3500'
