# Zybo RTOS Sensor Aggregator

A FreeRTOS sensor logger on the Digilent Zybo (Zynq-7000). A high-priority
task samples the Zynq's on-chip XADC (die temperature and supply rails) plus
the board's buttons/switches on a hardware timer tick, stores the samples in a
mutex-protected ring buffer in DDR, and a low-priority task formats and logs
them over UART. A hardware watchdog resets the board if any task stops making
progress.

What it exercises:

- task priorities, mutexes, queues, ISR-to-task signalling
- a ring buffer in DDR that a fast producer and slow consumer share safely
- hardware-timer driven sampling and the Zynq system watchdog (SWDT)

## Plan

Firmware first, entirely on the PS. The PL design comes last, once the software is done.

| Stage | Scope | Folder | Status |
|-------|-------|--------|--------|
| 1 | Bare-metal drivers: UART, MIO GPIO, XADC; temperature on the terminal | `sw/stage1_baremetal_drivers` | Code complete, board bring-up pending |
| 2 | FreeRTOS: timer-paced producer (100 ms XADC), consumer printing over UART | `sw/stage2_freertos_tasks` | Code complete, board bring-up pending |
| 3 | Ring buffer in DDR, mutex protection, queue between tasks | `sw/stage3_ringbuffer_sync` | Code complete, ring buffer host-tested, board bring-up pending |
| 4 | Zynq hardware watchdog + dedicated kick task, hang detection | - | Planned |
| 5 | PL design: AXI GPIO for SW0-3 / BTN0-3, full hardware platform | `hw/` | Planned |

Each stage has its own Readme with design notes, build steps and a bring-up checklist.

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
    Readme.md
    src/                         FreeRTOS application sources
    tests/host/                  unit tests that run on a PC
```

Each stage is a complete, self-contained application source tree, so any
stage can be built and run on its own. Drivers are carried forward from the
previous stage and changed in place; the commit history shows exactly what
changed and why.

Build outputs (Vivado project, XSA, Vitis workspace) are not tracked - they
are regenerated from the scripts and sources.

## Hardware and tools

- Digilent Zybo, Zybo Z7-10 or Zybo Z7-20
- Micro-USB cable on the PROG/UART port, serial terminal at 115200 8N1
- Vivado + Vitis (classic or Unified IDE; the sources handle both BSP flows)
- Digilent board files installed in Vivado

## References

- UG585 - Zynq-7000 SoC Technical Reference Manual (UART, GPIO, XADC interface, SWDT)
- UG480 - 7 Series FPGAs and Zynq-7000 SoC XADC User Guide
- Zybo / Zybo Z7 reference manuals (Digilent)
