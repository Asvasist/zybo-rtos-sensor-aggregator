# Stage 1 - Bare-metal drivers (UART, GPIO, XADC)

Status: **code complete, bring-up on the board pending**

The point of this stage is to get every peripheral the later stages depend on
working on its own, in a plain super-loop, before FreeRTOS is added. When
something misbehaves in stage 2 it's then an RTOS problem, not a driver one.

Exit criterion: the chip's die temperature is read from the XADC and printed on
the serial terminal, with the buttons and LED working alongside it.

## Board resources used

Everything here is on the PS side, so no bitstream is needed (see `hw/Readme.md`).

| Function | Zybo resource | Notes |
|----------|---------------|-------|
| Console | PS UART1, MIO48/49 | 115200 8N1, PROG/UART micro-USB |
| Heartbeat LED | LD4, MIO7 | toggles every 500 ms, fast blink = init failure |
| Button A | BTN4, MIO50 | full sensor report |
| Button B | BTN5, MIO51 | start/stop telemetry stream |
| Sensors | XADC via PS-XADC interface | die temp + 6 supply rails |

SW0-SW3 and BTN0-BTN3 are on PL pins and need AXI GPIO, so they arrive with the
PL design at the end. The button debouncer is table-driven so they can be added
without touching its logic.

## Source layout

```
src/
  main.c               super-loop, command handling, report formatting
  board/board_zybo.h   instance IDs, pin numbers, baud rate - the only board-specific file
  drivers/uart_drv.*   console UART, polled TX/RX, RX line-error counters
  drivers/gpio_drv.*   LD4 + BTN4/BTN5, integrating debouncer, latched press events
  drivers/xadc_drv.*   sequencer setup, raw reads, HW min/max, unit conversion
  app/console.*        printf over UART, \n -> \r\n, fixed-point number formatting
```

The drivers sit on top of the Xilinx standalone low-level drivers (XUartPs,
XGpioPs, XAdcPs) rather than raw register pokes. XAdcPs in particular hides the
XADCIF command/read FIFO handshake, which is easy to get subtly wrong and
gains nothing by being rewritten. What the project-level drivers add is the
configuration policy, error reporting, debouncing, and a small API that
won't change when the RTOS comes in.

## Design notes

**UART.** Polled. `uart_drv_init()` waits for the TX FIFO to drain first,
because ps7_init/FSBL already has the UART running as BSP stdout and changing
the baud generator mid-character garbles the tail of the boot output. Receive
overrun/framing/parity errors are counted from the raw interrupt status bits
(they latch even with the interrupts masked) - `d` on the terminal shows them.
`uart_drv_wait_tx_idle()` checks both TXEMPTY and TACTIVE; it's there for the
watchdog stage, so the last log line before a reset actually makes it out.

**GPIO.** Buttons are scanned every 10 ms. The debounced level only flips after
3 consecutive disagreeing samples (30 ms). A press is latched on the
released-to-pressed edge and cleared when the application takes it, so a
quick tap between two loop passes isn't lost and a held button doesn't repeat.
The debouncer is seeded from the live pin level at init, so a button held
through reset isn't reported as a press.

**XADC.** Configured once at init:
- self-test (register loopback + XADC reset) to prove the XADCIF link
- sequencer to safe mode, then channels and averaging selected
- factory calibration on (ADC gain/offset and supply gain/offset)
- 16-sample averaging on every measurement channel
- continuous sequencer mode over temp, VCCINT, VCCAUX, VCCBRAM, VCCPINT, VCCPAUX, VCCO_DDR

Alarm thresholds are left at their defaults - the over-temperature alarm drives
the automatic thermal shutdown and there's no reason to touch it yet.

Reads just return the latest averaged result from the status registers. Values
are kept as raw 12-bit codes and only converted for display, so a sample record
is 14 bytes; this is the format that will go into the ring buffer in stage 3.

Transfer functions (UG480), done in integer milli-units:

| Sensor | Formula | 1 LSB |
|--------|---------|-------|
| Temperature | `code * 503.975 / 4096 - 273.15` degC | ~0.123 degC |
| Supply rails | `code * 3.0 / 4096` V | ~0.73 mV |

No floating point is used anywhere, so printf never needs float support.

**Timing.** The loop uses the Cortex-A9 global timer (`XTime_GetTime`) for its
soft periods: 10 ms button scan, 500 ms heartbeat, 1 s telemetry. These are
not hard real-time - printing a full report blocks for ~70 ms at 115200 baud
and stretches them. That's accepted for stage 1; stage 2 moves sampling onto a
hardware timer interrupt.

## Build and run

### 1. Hardware platform

Generate `hw/export/zybo_ps_platform.xsa` as described in `hw/Readme.md`.

### 2a. Vitis Classic (2023.1 and earlier)

1. File > New > Application Project, "Create a new platform from hardware (XSA)",
   pick the XSA.
2. Processor `ps7_cortexa9_0`, OS `standalone`, template **Empty Application (C)**.
3. Copy the contents of `src/` into the application's `src/` folder
   (or right-click `src` > Import > File System).
4. Application project > Properties > C/C++ Build > Settings > ARM v7 gcc
   compiler > Directories, add:
   - `${ProjDirPath}/src/board`
   - `${ProjDirPath}/src/drivers`
   - `${ProjDirPath}/src/app`
5. Build.

### 2b. Vitis Unified (2023.2 and later)

1. Create a platform component from the XSA, standalone on `ps7_cortexa9_0`, build it.
2. Create an application component from the **Empty Application** example on that platform.
3. Copy `src/` into the component's `src/`.
4. In `src/UserConfig.cmake` add the three folders to `USER_INCLUDE_DIRECTORIES`:
   ```
   set(USER_INCLUDE_DIRECTORIES
       "${CMAKE_SOURCE_DIR}/board"
       "${CMAKE_SOURCE_DIR}/drivers"
       "${CMAKE_SOURCE_DIR}/app"
   )
   ```
   If the build doesn't pick up the `.c` files in the subfolders, list them in
   `USER_COMPILE_SOURCES` in the same file.
5. Build.

The board header handles both BSP flows - `LookupConfig()` takes a device ID
in the classic flow and a base address in the SDT flow.

### 3. Run

1. Jumper JP5 to JTAG, plug the PROG/UART micro-USB, power on.
2. Open a terminal on the board's COM port: 115200, 8N1, no flow control.
3. Run As > Launch Hardware (Classic) or Run (Unified).

## Terminal interface

| Key | Button | Action |
|-----|--------|--------|
| `r` | BTN4 | full report: all sensors, current + hardware min/max + raw code |
| `s` | BTN5 | start/stop the 1 s telemetry line (on at boot) |
| `t` | | one telemetry line now |
| `d` | | diagnostics: uptime, stream state, button levels, UART error counts |
| `h` / `?` | | help |

What it should look like (numbers will differ from board to board):

```
==================================================
 Zybo sensor aggregator - stage 1, bare-metal I/O
 fw 0.1.0, built Sep 11 2026 10:42:17
==================================================
XADC up: continuous sequencer, 16x averaging, calibration on

Commands:
  r / BTN4   full sensor report
  s / BTN5   start/stop 1 s telemetry stream
  t          one telemetry line now
  d          diagnostics
  h / ?      this help


[     0.412] XADC report (min/max tracked by XADC since reset)
  sensor           now        min        max   raw
  Die temp     44.91 C    44.62 C    45.11 C   0xA19
  VCCINT       1.001 V    0.999 V    1.003 V   0x557
  VCCAUX       1.797 V    1.795 V    1.799 V   0x996
  VCCBRAM      1.001 V    0.999 V    1.002 V   0x557
  VCCPINT      1.000 V    0.998 V    1.002 V   0x555
  VCCPAUX      1.798 V    1.796 V    1.800 V   0x997
  VCCO_DDR     1.348 V    1.347 V    1.351 V   0x731

[     1.413] T 44.91 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
[     2.413] T 44.99 C | VCCINT 1.001 V | VCCPINT 1.001 V | BTN4 0 BTN5 0
```

## Bring-up checklist

- [ ] Banner prints cleanly, no garbage after the FSBL output
- [ ] Die temperature is plausible (roughly 35-60 degC on an idle board in a room)
- [ ] Temperature rises when a fingertip is held on the Zynq, max tracks it
- [ ] Supply rails within a few percent of nominal
- [ ] LD4 blinks at 1 Hz
- [ ] BTN4 gives exactly one report per press, no doubles when tapping fast
- [ ] BTN5 toggles the stream, holding it down does not repeat
- [ ] Terminal keys work; `d` shows zero UART errors
- [ ] Wrong baud on the terminal side -> framing error count goes up in `d`

## Known limitations (on purpose, handled in later stages)

- Everything is polled from one loop; periods are soft and stretch while printing.
- Console output isn't thread-safe (single shared format buffer).
- `gpio_drv_btn_take_press()` is a plain read-then-clear; fine in one context,
  needs to be atomic once the scan runs from a timer ISR.
- No watchdog: a hang in the loop just stops the heartbeat LED.

## Next: stage 2

FreeRTOS on the same BSP. A hardware timer interrupt paces a high-priority
producer task that samples the XADC every 100 ms; a low-priority consumer task
formats and prints over the UART.
