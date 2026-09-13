# Zybo RTOS Sensor Aggregator

A FreeRTOS sensor logger on the Digilent Zybo (Zynq-7000). A high-priority
task samples the Zynq's on-chip XADC (die temperature and supply rails) plus
the board's PS push buttons on a hardware timer tick, stores the samples in a
mutex-protected ring buffer in DDR, and a low-priority task formats and logs
them over UART. A hardware watchdog resets the board if any task stops making
progress. The slide switches and PL buttons follow with the PL design.

What it exercises:

- task priorities, mutexes, queues, ISR-to-task signalling
- a ring buffer in DDR that a fast producer and slow consumer share safely
- hardware-timer driven sampling and the Zynq system watchdog (SWDT)

## Plan

Firmware first, entirely on the PS. The PL design comes last, once the software is done.

| Stage | Scope | Folder | Status |
|-------|-------|--------|--------|
| 1 | Bare-metal drivers: UART, MIO GPIO, XADC; temperature on the terminal | `sw/stage1_baremetal_drivers` | Code complete, builds against the 2025.2 BSP, board run pending |
| 2 | FreeRTOS: timer-paced producer (100 ms XADC), consumer printing over UART | `sw/stage2_freertos_tasks` | Code complete, builds against the 2025.2 BSP, board run pending |
| 3 | Ring buffer in DDR, mutex protection, queue between tasks | `sw/stage3_ringbuffer_sync` | First board run on a Zybo Z7-20 done, its two defects fixed, re-run pending |
| 4 | Zynq system watchdog, supervisor task with per-task check-ins, reset cause | `sw/stage4_watchdog` | Code complete, builds against the 2025.2 BSP, board run pending |
| 5 | PL design: AXI GPIO for SW0-3 / BTN0-3, full hardware platform | `hw/` | Later |

Stage 4 is the complete firmware - it contains everything from the earlier
stages. Each stage has its own Readme with design notes, build steps and a
bring-up checklist; the stage 3 Readme has the full Vivado/Vitis 2025.2
walkthrough, the stage 4 Readme adds the watchdog and SD boot parts.

## Quick start

1. Build the XSA: `hw/scripts/create_ps_platform.tcl` (see `hw/Readme.md`).
2. In Vitis 2025.2 create a FreeRTOS platform from it and an empty application
   (stage 3 Readme, *Running on the board*).
3. Copy `sw/stage4_watchdog/src` into the application's `src/` flat, build, run.
   Terminal at 115200 8N1, `h` for the commands.

## Repository layout

```
hw/
  Readme.md                      hardware platform notes
  scripts/create_ps_platform.tcl PS-only Vivado project + XSA export
sw/
  stage1_baremetal_drivers/
    Readme.md
    src/                         application sources (drop into a Vitis app)
  stage2_freertos_tasks/
    Readme.md
    src/                         FreeRTOS application sources
  stage3_ringbuffer_sync/
    Readme.md                    design, 2025.2 board walkthrough, bring-up log
    src/                         same layout as stage 4, without the watchdog
    tests/host/                  unit tests that run on a PC
  stage4_watchdog/
    Readme.md                    watchdog design, SD boot, tests
    src/
      main.c
      config/                    board map, application tuning
      drivers/                   UART, GPIO, XADC, TTC sample timer, SWDT, SLCR
      system/                    console, uptime, reset cause, fault handling, RTOS hooks
      datalog/                   sample record, ring buffer, sensor log
      tasks/                     watchdog supervisor, producer, consumer, UI
    tests/host/                  unit tests that run on a PC
    boot/boot.bif                SD card boot image (FSBL + application)
```

Vitis 2025.2 only compiles sources sitting directly in an application's
`src/` folder, so a stage's sources are copied in flat. The walkthrough in
the stage 3 Readme shows how.

Each stage is a complete, self-contained application source tree, so any
stage can be built and run on its own. Drivers are carried forward from the
previous stage and changed in place; the commit history shows exactly what
changed and why.

Build outputs (Vivado project, XSA, Vitis workspace) are not tracked - they
are regenerated from the scripts and sources.

## Hardware and tools

- Digilent Zybo, Zybo Z7-10 or Zybo Z7-20
- Micro-USB cable on the PROG/UART port, serial terminal at 115200 8N1
- Vivado + Vitis (classic or Unified IDE; the sources handle both BSP flows).
  Brought up with 2025.2 on a Zybo Z7-20.
- Digilent board files installed in Vivado

## References

- UG585 - Zynq-7000 SoC Technical Reference Manual (UART, GPIO, XADC interface, SWDT)
- UG480 - 7 Series FPGAs and Zynq-7000 SoC XADC User Guide
- Zybo / Zybo Z7 reference manuals (Digilent)
