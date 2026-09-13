# Hardware

## Current state: PS-only platform

Stages 1 to 4 run entirely on the Zynq PS, so the hardware side is just the
PS7 block with the Zybo board preset - no PL logic and no bitstream. Vitis
still needs an `.xsa` to build the platform/BSP from, and that's all this
folder produces for now.

The PL design (AXI GPIO for the slide switches and the PL push buttons, and
whatever else the final stage needs) is done after the firmware is complete.

## Generating the XSA

Requires Vivado with the Digilent board files installed.

```
cd hw
vivado -mode batch -source scripts/create_ps_platform.tcl -tclargs zybo-z7-10
```

Board argument is one of `zybo`, `zybo-z7-10`, `zybo-z7-20`.

Output:

| Path | What |
|------|------|
| `hw/build/zybo_ps_platform/` | throwaway Vivado project (git-ignored) |
| `hw/export/zybo_ps_platform.xsa` | hardware handoff for Vitis (git-ignored, regenerate it) |

## What the platform gives the firmware

| Resource | Where | Used by | Since |
|----------|-------|---------|-------|
| UART1 | MIO48 TX / MIO49 RX, USB-UART on the PROG/UART port | console | stage 1 |
| GPIO | MIO7 (LD4), MIO50 (BTN4), MIO51 (BTN5) | heartbeat LED, buttons | stage 1 |
| XADC | PS-XADC interface (DevC), no PL wiring needed | temperature, supply rails | stage 1 |
| TTC0 | counter 2 / IRQ 44 from stage 3 (counter 1 / IRQ 43 in stage 2), outputs on EMIO (unused) | 100 ms sample timer | stage 2 |
| DDR3 | 512 MB (original Zybo), 1 GB (Zybo Z7) | code, data, sensor log (stage 3) | - |

Everything except TTC0 and the button pad pull-ups comes from the board
preset. The script switches TTC0 on and disables the internal pull-ups on
MIO50/51. The Zybo Z7 preset leaves those enabled, which makes BTN4/BTN5 read
as permanently pressed. The firmware clears them at boot as well, so an older
XSA still works. An XSA exported before stage 2 may not have it -
the build then fails on the missing `XPAR_XTTCPS_*` definitions, and
regenerating the XSA fixes it.

With a FreeRTOS BSP in the SDT flow (Vitis 2023.2 and later) one TTC0 counter
becomes the RTOS tick by default - that's why stage 3 samples on counter 2.
The stage 3 Readme has the full Vivado and Vitis 2025.2 walkthrough.

If the project is built by hand in the GUI instead of the script: add a ZYNQ7
Processing System, run block automation with "Apply Board Preset" ticked,
untick M_AXI_GP0 under PS-PL configuration, tick TTC0 under MIO configuration
> Application Processor Unit > Timer 0, set Pullup to *disabled* for MIO 50
and 51 in the MIO configuration table, create the HDL wrapper and export the
hardware (no bitstream). The step-by-step version is in the stage 3 Readme.
