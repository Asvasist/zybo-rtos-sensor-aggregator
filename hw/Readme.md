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

## What the preset gives the firmware

| Resource | Where | Used by |
|----------|-------|---------|
| UART1 | MIO48 TX / MIO49 RX, USB-UART on the PROG/UART port | console |
| GPIO | MIO7 (LD4), MIO50 (BTN4), MIO51 (BTN5) | heartbeat LED, buttons |
| XADC | PS-XADC interface (DevC), no PL wiring needed | temperature, supply rails |
| DDR3 | 512 MB (original Zybo), 1 GB (Zybo Z7) | code, data, ring buffer later |

If the project is built by hand in the GUI instead of the script: add a ZYNQ7
Processing System, run block automation with "Apply Board Preset" ticked,
untick M_AXI_GP0 under PS-PL configuration, create the HDL wrapper and export
the hardware (no bitstream).
