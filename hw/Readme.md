# Hardware

## Current state: PS-only platform

All four firmware stages run on the Zynq PS alone, so the hardware side is
just the PS7 block with the Zybo board preset and a few settings changed. No
PL logic and no bitstream. Vitis still needs an `.xsa` to build the
platform/BSP from, and that's all this folder produces for now.

The PL design (AXI GPIO for the slide switches and the PL push buttons) is
the next step now that the firmware is done.

## Generating the XSA with the script

Needs Vivado with the Digilent board files installed. From the `hw` folder:

```
vivado -mode batch -source scripts/create_ps_platform.tcl -tclargs zybo-z7-20
```

The board argument is `zybo`, `zybo-z7-10` or `zybo-z7-20` (default `zybo-z7-20`,
the board this was brought up on). It takes a couple of minutes.

| Output | What |
|--------|------|
| `hw/build/zybo_ps_platform/` | the Vivado project (git-ignored, throwaway) |
| `hw/export/zybo_ps_platform.xsa` | hardware handoff for Vitis (git-ignored, regenerate it) |

Last checked with Vivado 2025.2 on the Z7-20: it finishes without errors. The
log shows four critical warnings about negative `PCW_UIPARAM_DDR_DQS_TO_CLK_DELAY`
values. Those come from Digilent's board preset (the board's DDR trace delays),
not from the script, and they're expected. The pile of `Board 49-26` warnings
about Xilinx eval boards is Vivado listing boards the free edition can't use -
also harmless.

If you'd rather click through it by hand, or you already have a project from
an earlier stage, the stage 4 Readme has the step-by-step.

## What the platform gives the firmware

| Resource | Where | Used by | Since |
|----------|-------|---------|-------|
| UART1 | MIO48 TX / MIO49 RX, USB-UART on the PROG/UART port | console | stage 1 |
| GPIO | MIO7 (LD4), MIO50 (BTN4), MIO51 (BTN5) | heartbeat LED, buttons | stage 1 |
| XADC | PS-XADC interface (DevC), no PL wiring needed | temperature, supply rails | stage 1 |
| TTC0 | counter 2 / IRQ 44 from stage 3 (counter 1 / IRQ 43 in stage 2), outputs on EMIO (unused) | 100 ms sample timer | stage 2 |
| SWDT | system watchdog, reset goes to the PS internally, EMIO outputs unused | board reset on a hang | stage 4 |
| DDR3 | 512 MB (original Zybo), 1 GB (Zybo Z7) | code, data, sensor log | - |

Settings the script changes on top of the board preset:

| Setting | Value | Why |
|---------|-------|-----|
| M_AXI_GP0 | off | nothing in the PL to talk to, avoids an undriven-clock validation error |
| FCLK_CLK0, FCLK_RESET0 | off | no PL logic to clock |
| TTC0 | on, IO EMIO | sample timer |
| Watchdog (SWDT) | on, IO EMIO | stage 4 |
| MIO 50 / 51 pull-up | disabled | the Z7 preset enables them and BTN4/BTN5 then read as pressed |

The firmware also clears the MIO 50/51 pull-ups at boot, so an XSA without
that change still works. An XSA that's missing TTC0 or the SWDT makes the
application build fail on `XPAR_XTTCPS_*` / `XPAR_XWDTPS_*` - regenerate it.

With a FreeRTOS BSP in the SDT flow (Vitis 2023.2 and later) one TTC0 counter
can become the RTOS tick; that's why the firmware samples on counter 2.
