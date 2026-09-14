# Zybo RTOS Sensor Aggregator

A FreeRTOS sensor logger on the Digilent Zybo (Zynq-7000). A high-priority
task samples the Zynq's on-chip XADC (die temperature and supply rails) and
the board's PS push buttons on a hardware timer tick. It stores the samples in
a mutex-protected ring buffer in DDR, and a low-priority task formats them and
prints them over UART. A hardware watchdog resets the board if any task stops
making progress.

The slide switches and the PL buttons come later, with the PL design.

What it covers:

- task priorities, mutexes, queues, getting from an ISR to a task
- a ring buffer in DDR that a fast producer and a slow consumer share safely
- sampling paced by a hardware timer, and the Zynq system watchdog (SWDT)

## Stages

Firmware first, entirely on the PS. The PL design comes after.

| Stage | Scope | Folder | Status |
|-------|-------|--------|--------|
| 1 | Bare-metal drivers: UART, MIO GPIO, XADC; temperature on the terminal | `sw/stage1_baremetal_drivers` | Code complete, builds against the 2025.2 BSP, board run pending |
| 2 | FreeRTOS: timer-paced producer (100 ms XADC), consumer printing over UART | `sw/stage2_freertos_tasks` | Code complete, builds against the 2025.2 BSP, board run pending |
| 3 | Ring buffer in DDR, mutex protection, queue between tasks | `sw/stage3_ringbuffer_sync` | First board run on a Zybo Z7-20 done, its two defects fixed, re-run pending |
| 4 | Zynq system watchdog, supervisor task with per-task check-ins, reset cause | `sw/stage4_watchdog` | Code complete, built end to end with Vivado/Vitis 2025.2, board run pending |
| 5 | PL design: AXI GPIO for SW0-3 / BTN0-3 | `hw/` | Later |

Stage 4 is the complete firmware - everything from the earlier stages is in
it. The earlier stage folders are kept as they were, so each step can still be
built and read on its own.

Each stage has its own Readme. The stage 3 Readme has the full Vivado/Vitis
2025.2 walkthrough; the stage 4 Readme adds the watchdog, updating an existing
platform, and booting from SD.

## Quick start

1. **XSA.** From the `hw` folder:
   `vivado -mode batch -source scripts/create_ps_platform.tcl -tclargs zybo-z7-20`.
   Or build it by hand - see the stage 4 Readme, *Hardware: enabling the watchdog*.
2. **Vitis 2025.2.** Create a FreeRTOS platform for `ps7_cortexa9_0` from
   `hw/export/zybo_ps_platform.xsa`, then an empty application on it (stage 3
   Readme, *Running on the board*).
3. **Sources.** Copy every `.c`/`.h` from `sw/stage4_watchdog/src` flat into the
   application's `src/` folder, build, and run. Terminal at 115200 8N1, `h` for
   the commands.

## Repository layout

```
hw/
  Readme.md                      what the platform contains and why
  scripts/create_ps_platform.tcl PS-only Vivado project + XSA export
sw/
  stage1_baremetal_drivers/
    Readme.md
    src/                         bare-metal application sources
  stage2_freertos_tasks/
    Readme.md
    src/                         FreeRTOS application sources
  stage3_ringbuffer_sync/
    Readme.md                    design, 2025.2 board walkthrough, bring-up log
    src/                         same layout as stage 4, without the watchdog
    tests/host/                  unit tests that run on a PC
  stage4_watchdog/
    Readme.md                    watchdog, updating the platform, SD boot, tests
    src/
      main.c
      config/                    board map, application settings
      drivers/                   UART, GPIO, XADC, TTC sample timer, SWDT, SLCR
      system/                    console, uptime, reset cause, fault handling, RTOS hooks
      datalog/                   sample record, ring buffer, sensor log
      tasks/                     watchdog supervisor, producer, consumer, UI
    tests/host/                  unit tests that run on a PC
    boot/boot.bif                SD card boot image (FSBL + application)
```

Vitis 2025.2 only compiles sources that sit directly in an application's
`src/` folder, so a stage's sources are copied in flat. The folders are for
reading the code; the includes are by file name, so flattening doesn't break
anything.

Drivers are carried forward from one stage to the next and changed in place.
The commit history shows what changed and why.

Build outputs aren't tracked (Vivado project, XSA, Vitis workspace,
`BOOT.BIN`); they're regenerated from the scripts and sources.

## Hardware and tools

- Digilent Zybo, Zybo Z7-10 or Zybo Z7-20 (brought up on a Z7-20)
- Micro-USB cable on the PROG/UART port, serial terminal at 115200 8N1
- microSD card for the watchdog reboot test (optional)
- Vivado + Vitis 2025.2, with the Digilent board files. The sources still
  compile for the older classic Vitis flow, but that isn't tested on a board.

## References

- UG585 - Zynq-7000 SoC Technical Reference Manual (UART, GPIO, XADC interface, TTC, SWDT, SLCR)
- UG480 - 7 Series FPGAs and Zynq-7000 SoC XADC User Guide
- Zybo / Zybo Z7 reference manuals (Digilent)
