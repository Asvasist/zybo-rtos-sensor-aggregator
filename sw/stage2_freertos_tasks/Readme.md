# Stage 2 - FreeRTOS task architecture

Status: **code complete, bring-up on the board pending**

Stage 1's super-loop is replaced by FreeRTOS tasks. A hardware timer interrupt
paces a high-priority **producer** that reads the XADC every 100 ms, and a
low-priority **consumer** formats the data and sends it over the UART. A third
task handles buttons and terminal keys.

Exit criterion: samples arrive at a steady 100 ms set by the hardware timer,
and that timing holds no matter what the consumer is doing (printing long
reports, falling behind, or being stalled on purpose).

## What changed from stage 1

- The super-loop is gone. There are now three tasks: producer, UI and consumer.
- New `sample_timer` driver: TTC0 counter 2 in interval mode, interrupt at 10 Hz.
- New `sensor_mailbox`: a single slot that carries the newest sample from the producer to the consumer.
- New `fault` module: init failures and RTOS hooks all end in the same place.
- The drivers came over from stage 1 with small changes (the first commit on this branch is a verbatim copy, so `git diff` shows exactly what changed):
  - `xadc_drv`: no more `usleep()` waiting for the first conversion in init.
  - `uart_drv`: the error counter snapshot no longer touches the hardware, so only the RX owner reads and clears the status flags.
  - `gpio_drv`, `console`: comments only, documenting which task owns what.
- `board_zybo.h`: TTC instance and IRQ number added.
- `hw/scripts`: TTC0 is now enabled explicitly. **Regenerate the XSA.**

## Task architecture

| Task | Priority | Stack | Wakes on | Owns |
|------|----------|-------|----------|------|
| producer | idle + 4 | 512 words | TTC0 interrupt, every 100 ms | XADC, sample timer |
| ui | idle + 2 | 512 words | `vTaskDelayUntil`, every 10 ms | button scan, UART RX |
| consumer | idle + 1 | 1024 words | notification bits | UART TX, LD4 |

```
                 IRQ 44
  TTC0 counter 2 ------> sample_timer_isr()
                                |
                                | xTaskNotifyGiveFromISR()
                                v
                         +-------------+   XADCIF   +------+
                         |  producer   |<---------->| XADC |
                         |  prio 4     |            +------+
                         +-------------+
             post (critical |       | xTaskNotify(EVT_SAMPLE)
               section)     v       |
                  +----------------+|
                  | sensor_mailbox ||
                  | newest only    ||
                  +----------------+|
                    fetch |         v
                          |  +-------------+  polled TX  +--------+
                          +->|  consumer   |------------>|  UART  |
                             |  prio 1     |             +--------+
                             +-------------+                 |
                                    ^                        | RX
                                    | xTaskNotify(EVT_...)   |
                             +-------------+                 |
         BTN4 / BTN5 ------->|     ui      |<----------------+
                             |  prio 2     |
                             +-------------+
```

### Life of one sample

1. TTC0 counter 2 reaches its match value and raises IRQ 44.
2. `sample_timer_isr()` reads (and so clears) the TTC status, then calls the
   producer's callback. The callback calls `xTaskNotifyGiveFromISR()` and
   `portYIELD_FROM_ISR()`, so the port switches straight to the producer on IRQ
   exit instead of waiting for the next RTOS tick.
3. The producer timestamps the wake-up (global timer, microseconds), reads all
   7 XADC channels plus the debounced button levels, posts the record to the
   mailbox, sets `CONSUMER_EVT_SAMPLE`, updates its statistics and blocks again.
4. The consumer runs whenever nothing more important is ready. It fetches the
   newest record, toggles LD4 every 5th sample, and prints a line if the
   stream is on.

## Design notes

**Why a TTC and not `vTaskDelayUntil()`.** The sample period comes from a
divider on a crystal-derived clock, not from the RTOS tick, so it doesn't
change with `configTICK_RATE_HZ`. The ISR also wakes the producer the moment
the period expires rather than at the next tick boundary. The integer divider
can make it differ slightly from exactly 100 ms; `d` shows the real period (100000 us on the Zybo Z7).

**Why the ISR only notifies.** Reading the XADC takes several command/response
exchanges over the XADCIF FIFOs. Doing that in IRQ context would keep
interrupts blocked for all of that time. Here the ISR is a handful of
instructions, and the slow part runs at task level where it can be pre-empted
if anything more urgent ever needs to be.

**Why counting notifications.** `ulTaskNotifyTake(pdTRUE, ...)` returns how many
ticks were pending. If it returns 2, a tick arrived while the producer was
still busy with the previous sample. That is counted as an **overrun**, and it
should never happen. If it returns 0 after 300 ms, the timer has stopped
firing and a **timeout** is counted.

**Overruns vs. missed samples.** These are two different failures and are
counted separately:
- *overrun* (producer): the producer itself was late. This is a real-time failure.
- *missed* (consumer): the consumer didn't get to a sample before the next one
  overwrote it. That is expected when the consumer is slow, and it is exactly
  what stage 3's ring buffer fixes. It is computed as `latest seq - samples received`.

**Ownership instead of locks.** Stage 2 has no mutexes on purpose. Each shared
resource has exactly one task that uses it:

| Resource | Owner | Anyone else |
|----------|-------|-------------|
| XADC | producer | nobody (the XADCIF exchange isn't reentrant) |
| sample timer | producer | ISR (reads/clears the status only) |
| UART TX + console format buffer | consumer | `main()` before the scheduler starts; `fault_halt()` with IRQs masked |
| UART RX + line error flags | ui | consumer reads a counter snapshot |
| button scan / press latches | ui | producer reads the debounced levels (single bools) |
| LD4 | consumer | `fault_halt()` |
| producer statistics | producer | consumer copies them inside a critical section |

Stage 3 introduces real sharing (a buffer with a writer and a reader that can
block) and mutexes come in with it.

**Mailbox critical section.** Posting and fetching copy ~32 bytes with
interrupts masked, which takes a few microseconds. A mutex would cost more
than that, and it would allow the consumer to block the producer. For a single
slot the critical section is the right tool. For a buffer of history it isn't,
which is why stage 3 changes it.

**Notification bits.** The consumer waits on one notification value used as
event bits (`CONSUMER_EVT_*`). Bits don't count, so an event posted twice
before the consumer runs is handled once. For samples that doesn't matter (the
consumer always fetches the newest). For UI requests, a very fast double
press can collapse into one. Stage 3 moves commands onto a queue.

**FPU context.** GCC and newlib's `memcpy` may use VFP registers even in
integer-only code (struct copies are the usual culprit), so every task calls
`portTASK_USES_FLOATING_POINT()` first. The ISR path stays free of library
calls and floating point, because the port doesn't save the VFP registers on
IRQ entry.

**Interrupt priority.** The TTC interrupt keeps the GIC default priority
(0xA0) and the level-sensitive trigger. That priority is below the port's
`configMAX_API_CALL_INTERRUPT_PRIORITY` limit, so the ISR may call `FromISR`
functions. If it ever ends up wrong, the port's `configASSERT` catches it.

**Startup order.** All peripheral init that doesn't need interrupts happens
in `main()`, single-threaded. The TTC and its interrupt are set up at the top
of the producer task, because the Xilinx port creates the GIC instance inside
`vTaskStartScheduler()`. Connecting a handler before that either fails or gets
wiped out when the port initializes the GIC.

**Fatal errors.** Init failures, a stack overflow and a failed allocation all
end up in `fault_halt()`. It masks IRQs at the CPU, prints with
`console_write()` only (no printf, since a task stack may be corrupt), then
flashes LD4 at 5 Hz forever. Stage 4 replaces "halt" with a watchdog reset.

## FreeRTOS BSP settings

The defaults of the Xilinx FreeRTOS BSP work. These are the settings the code
relies on. The ones marked *checked* fail the build if they're wrong.

| FreeRTOSConfig.h | Value | Why |
|------------------|-------|-----|
| `configMAX_PRIORITIES` | >= 5 | producer runs at idle + 4 (*checked*) |
| `configTICK_RATE_HZ` | 100 (default) or 1000 | UI scan needs >= 1 tick per 10 ms (*checked*) |
| `configUSE_TASK_NOTIFICATIONS` | 1 (default) | all task signalling |
| `INCLUDE_vTaskDelayUntil` | 1 | UI scan period |
| `configCHECK_FOR_STACK_OVERFLOW` | 2 | stack overflow hook |
| `configUSE_MALLOC_FAILED_HOOK` | 1 | malloc failed hook |
| `INCLUDE_uxTaskGetStackHighWaterMark` | 1 | stack headroom in `d` (the line is left out otherwise) |
| heap implementation | heap_4 (default) | `xPortGetFreeHeapSize()` doesn't exist in heap_3 |

In the classic Vitis flow these are under Board Support Package Settings >
freertos10_xilinx (kernel_behavior, kernel_features, hook_functions). In the
Unified IDE they are in the platform's BSP configuration under the freertos
section.

## Source layout

```
src/
  main.c                  peripheral init, task creation, scheduler start
  board/board_zybo.h      instance IDs, pins, TTC instance and IRQ
  drivers/
    uart_drv.*            console UART (from stage 1)
    gpio_drv.*            LD4, BTN4/BTN5 debouncer (from stage 1)
    xadc_drv.*            XADC sequencer and readout (from stage 1)
    sample_timer.*        TTC0 interval timer + interrupt hookup     [new]
  app/
    app_config.h          rates, priorities, stack sizes              [new]
    task_producer.*       timer-paced XADC sampling, statistics       [new]
    task_consumer.*       output formatting, the only console writer  [new]
    task_ui.*             buttons and terminal keys -> consumer events [new]
    sensor_mailbox.*      newest-sample handoff                       [new]
    fault.*               last-resort error handling                  [new]
    rtos_hooks.c          stack overflow / malloc failed hooks        [new]
    uptime.*              microsecond timestamps from the global timer [new]
    console.*             printf over UART (from stage 1)
```

## Fixes from the first board bring-up (Vitis 2025.2)

These were found while bringing up stage 3 on a Zybo Z7-20 with Vivado and
Vitis 2025.2 (SDT flow). The same code lives in this stage, so the fixes were
carried back here. This stage builds and links cleanly against that FreeRTOS
platform; a run on the board is still to do.

- `XUartPsFormat` and `vTaskNotifyGiveFromISR()`: the names used before didn't
  exist in the real driver and kernel headers.
- **Zero timestamps.** The SDT xiltimer library only starts the Cortex-A9
  global timer on the first sleep call. `uptime_init()` now starts it at boot.
- **Buttons stuck at 1.** The Z7 preset enables the MIO50/51 internal pull-ups.
  The firmware now sets the pad pull-up itself (`board_zybo.h`).
- **Sample timer moved to TTC0 counter 2.** In the SDT flow the interrupt ID
  comes from the TTC config table, and a counter that is already running is
  refused rather than taken over.
- XADC lookup in the SDT flow by base address 0 ("first instance").

The full Vivado and Vitis 2025.2 walkthrough is in
`sw/stage3_ringbuffer_sync/Readme.md`, and it applies to this stage unchanged.

## Build and run

### 1. Hardware platform

Regenerate `hw/export/zybo_ps_platform.xsa` (see `hw/Readme.md`). It needs
TTC0, which the stage 1 XSA may not have.

### 2a. Vitis Classic (2023.1 and earlier)

1. File > New > Application Project, create a new platform from the XSA.
2. Processor `ps7_cortexa9_0`, OS **freertos10_xilinx**, template
   **FreeRTOS Hello World**.
3. Delete `freertos_hello_world.c` from the generated `src/`, then copy the
   contents of this stage's `src/` in.
4. Application project > Properties > C/C++ Build > Settings > ARM v7 gcc
   compiler > Directories, add:
   - `${ProjDirPath}/src/board`
   - `${ProjDirPath}/src/drivers`
   - `${ProjDirPath}/src/app`
5. Check the BSP settings above, then build.

### 2b. Vitis Unified (2023.2 and later)

1. Create a platform component from the XSA with OS **freertos** on
   `ps7_cortexa9_0`, and build it.
2. Create an application component from the **FreeRTOS Hello World** example
   on that platform, and remove its example source.
3. Copy `src/` into the component's `src/`.
4. In `src/UserConfig.cmake` add the include folders:
   ```
   set(USER_INCLUDE_DIRECTORIES
       "${CMAKE_SOURCE_DIR}/board"
       "${CMAKE_SOURCE_DIR}/drivers"
       "${CMAKE_SOURCE_DIR}/app"
   )
   ```
   If the `.c` files in the subfolders aren't picked up, list them in
   `USER_COMPILE_SOURCES`.
5. Build.

### 3. Run

1. Set jumper JP5 to JTAG, plug in the PROG/UART micro-USB, power on.
2. Terminal on the board's COM port: 115200, 8N1, no flow control.
3. Run As > Launch Hardware (Classic) or Run (Unified).

## Terminal interface

| Key | Button | Action |
|-----|--------|--------|
| `r` | BTN4 | full report: latest sample for all sensors + min/max over all samples |
| `s` | BTN5 | start/stop the telemetry stream (on at boot) |
| `f` | | stream rate: every sample (10 Hz) / every 10th (1 Hz) |
| `t` | | print the latest sample once, even with the stream off |
| `d` | | diagnostics: timer period, overruns, interval min/max, missed samples, stacks, heap |
| `h` / `?` | | help |

Expected startup and stream (sensor values vary from board to board):

```
==================================================
 Zybo sensor aggregator - stage 2, FreeRTOS tasks
 fw 0.2.0, built Sep 12 2026 09:15:40
==================================================
XADC up: continuous sequencer, 16x averaging, calibration on
tasks created, starting scheduler
scheduler running, sample period 100000 us

Commands:
  r / BTN4   full sensor report (latest sample + min/max)
  s / BTN5   start/stop telemetry stream
  f          stream rate: every sample (10 Hz) / every 10th (1 Hz)
  t          print the latest sample once
  d          diagnostics (timing, missed samples, stacks, heap)
  h / ?      this help

[     0.518] #1      T 44.91 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
[     0.618] #2      T 44.91 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
[     0.718] #3      T 45.03 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
[     0.818] #4      T 44.91 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
```

Diagnostics layout (fill in the real numbers during bring-up):

```
Diagnostics
  uptime          : 42.731 s
  sample period   : 100000 us (timer)
  producer        : 422 samples, 0 overruns, 0 timeouts
  sample interval : min <n> us, max <n> us
  XADC read time  : max <n> us
  consumer        : 422 received, 0 missed, stream on, every sample
  UART rx errors  : overrun 0, framing 0, parity 0
  stack headroom  : producer <n>, ui <n>, consumer <n> words
  heap free       : <n> bytes
```

## Bring-up checklist

Basic operation
- [ ] Banner, "tasks created" and "scheduler running" lines print cleanly
- [ ] Stream timestamps step by 100 ms, sequence numbers have no gaps
- [ ] LD4 blinks at 1 Hz (this proves the whole timer -> ISR -> producer -> consumer chain)
- [ ] `d`: overruns and timeouts are 0; sample interval min/max stays well within 1 ms of the period
- [ ] `d`: note down the XADC read time - it sets how much of the 100 ms the producer really uses

Priorities
- [ ] Press `r` repeatedly while streaming: the reports print, and the interval max in `d` doesn't move
- [ ] BTN5 / `s` stops the stream, LD4 keeps blinking (samples still flow)
- [ ] `f` switches between 10 lines/s and 1 line/s

Deliberate failures (revert each one afterwards)
- [ ] Slow consumer: add `vTaskDelay(pdMS_TO_TICKS(250));` at the end of
      `consumer_handle_sample()`. Expected: sequence numbers in the stream jump by 2-3,
      `missed` climbs accordingly, while the producer interval and overruns stay unchanged
- [ ] Dead timer: add `if (record.seq == 50U) { sample_timer_stop(); }` after `record.seq++`
      in the producer. Expected: WARN line after 1 s, LD4 stops, timeouts climb in `d`
- [ ] Stack overflow: set `APP_STACK_CONSUMER` to 128. Expected:
      `FATAL: stack overflow in task: consumer`, LD4 flashing fast. A data abort
      instead is also possible - the overflow check only runs at context switches
- [ ] Stack headroom in `d` after several minutes: trim the stack sizes in `app_config.h` if there's lots to spare

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Build error on `XPAR_XTTCPS_2_*` | TTC0 not enabled in the XSA - regenerate it with the current script |
| Link error: multiple definition of `vApplicationStackOverflowHook` | this BSP version doesn't define its hooks weak - remove `app/rtos_hooks.c` |
| Link error: undefined `xPortGetFreeHeapSize` | heap_3 selected in the BSP - switch to heap_4 |
| `FATAL: sample timer init failed, code 5` | interrupt handler couldn't be installed on the GIC |
| `FATAL: sample timer init failed, code 6` | counter already running - xiltimer tick_timer is set to `ps7_ttc_2`, change it |
| Undefined references at link time | Vitis 2025.2 only builds `src/*.c` - copy the sources into `src/` flat, not in subfolders |
| No stream, `WARN: no sample for 1000 ms` | TTC not counting or IRQ not reaching the GIC - check IRQ 44 and TTC0 in the XSA |
| Garbage or data abort soon after "starting scheduler" | a task missing `portTASK_USES_FLOATING_POINT()`, or a stack too small |

## Known limitations (on purpose, handled in later stages)

- The mailbox holds only the newest sample, so a slow consumer loses data.
  Stage 3: ring buffer in DDR, mutex, queue.
- UI requests are notification bits and can coalesce. Stage 3: command queue.
- The single-writer console is a convention, not enforced. Stage 3: mutex.
- Faults halt the board instead of recovering, and a hung task isn't detected. Stage 4: watchdog.
- UART TX is polled, so the consumer busy-waits on the FIFO. That's harmless
  at the lowest priority, but it still costs CPU time the idle task would
  otherwise get.

## Next: stage 3

Keep every sample: the producer writes into a ring buffer in DDR protected by
a FreeRTOS mutex, and the consumer drains it at its own pace. A queue carries
messages between the tasks.
