# Stage 3 - Ring buffer in DDR, mutex, message queue

Status: **code complete, ring buffer unit tested on the host, bring-up on the board pending**

Stage 2 handed samples over through a single "newest value" slot, so a slow
consumer lost data. Now every sample goes into a **ring buffer in DDR** that
the producer writes and the consumer drains. Both access it through a
**FreeRTOS mutex**. Control flows through a **message queue**: UI commands, and
a doorbell from the producer when new data is waiting.

Exit criterion: the consumer can be paused, slowed down or made to hog the
lock, and the producer still samples every 100 ms on time. Every sample the
producer took is either printed or explicitly counted as lost, and the counts
add up.

## What changed from stage 2

- New `ring_buffer`: overwrite-oldest FIFO of sensor records, power-of-two
  capacity, no locking, no target dependencies. Unit tested on a PC (`tests/host`).
- New `sensor_log`: 4096-record storage in DDR (`.bss.sensor_log`) plus the ring
  buffer and a FreeRTOS mutex. This is the only way into the buffer.
- `sensor_mailbox` removed. `sensor_record_t` moved to its own header.
- Producer: writes every sample into the log with a bounded mutex wait, and
  sends a `DATA_READY` doorbell on the empty-to-non-empty edge.
- Consumer: blocks on a message queue (16 x `consumer_msg_t`) instead of
  notification bits, and drains the log in batches with the mutex released
  while it prints.
- UI: sends typed messages. Unknown keys carry the character. A full queue
  drops the message and counts it.
- Sample timer moved from TTC0 counter 1 to **counter 2** (GIC ID 44). With a
  FreeRTOS BSP in the SDT flow (Vitis 2023.2 and later), xiltimer claims
  counter 1 as the RTOS tick by default.
- `sample_timer`: in the SDT flow the interrupt ID now comes from the TTC
  config table (encoded SPI number), not the plain GIC ID, which the SDT
  interrupt wrapper would have offset by 32. The driver also refuses a counter
  that is already running (`SAMPLE_TIMER_ERR_BUSY`) instead of taking it over.
- XADC lookup in the SDT flow uses base address 0 ("first instance"), the same
  as the Xilinx SDT examples.
- New terminal command `p`: pause/resume reading the log, to exercise the buffer.
- Diagnostics (`d`) extended with log, mutex and queue statistics.
- `board_zybo.h`: DDR address window, used to verify where the log landed.
- Drivers, sample timer, fault handler and hooks are unchanged apart from comments.
- Hardware: no change from stage 2.

## Architecture

| Task | Priority | Stack | Wakes on | Owns |
|------|----------|-------|----------|------|
| producer | idle + 4 | 512 words | TTC0 interrupt, every 100 ms | XADC, sample timer, log writes |
| ui | idle + 2 | 512 words | `vTaskDelayUntil`, every 10 ms | button scan, UART RX |
| consumer | idle + 1 | 1024 words | its queue (or 250 ms poll) | UART TX (gatekeeper), LD4, log reads |

| Shared object | Kind | Size | Guarded by |
|---------------|------|------|------------|
| sensor log | ring buffer of `sensor_record_t` in DDR | 4096 x 32 B = 128 KiB | FreeRTOS mutex (priority inheritance) |
| consumer queue | FreeRTOS queue of `consumer_msg_t` | 16 x 8 B | the queue itself |
| producer statistics | struct | ~70 B | critical section (short copy, as in stage 2) |

```
                 IRQ 44
  TTC0 counter 2 ------> sample_timer_isr()
                                | xTaskNotifyGiveFromISR()
                                v
                         +-------------+   XADCIF   +------+
                         |  producer   |<---------->| XADC |
                         |  prio 4     |            +------+
                         +-------------+
            write (mutex,   |       |
            wait <= 20 ms)  |       | DATA_READY (only when the log was empty)
                            v       |
   +--------------------------------+---+
   |  sensor log - DDR, .bss.sensor_log |      +----------------------+
   |  ring buffer, 4096 records         |      |  consumer queue (16) |
   |  [ mutex ]                         |      +----------------------+
   +------------------------------------+        ^            |
                            |                    |  commands  |
             read, batches  |             +-------------+     |
             of 16 (mutex)  |             |     ui      |     |
                            v             |  prio 2     |     |
                         +-------------+  +-------------+     |
                         |  consumer   |<--------------------+
                         |  prio 1     |     BTN4/5, UART RX
                         +-------------+
                                |  polled TX
                                v
                             UART
```

### Life of one sample

1. TTC0 fires. The ISR wakes the producer, exactly as in stage 2.
2. The producer timestamps the sample, reads the XADC and buttons, and bumps `seq`.
3. `sensor_log_write()` takes the mutex, waiting at most 20 ms. It notes
   whether the log was empty, pushes the record (overwriting the oldest if the
   log is full), and gives the mutex back. If the mutex didn't come free in
   time, the sample is dropped and counted, and the next sample shows a gap.
4. If the log was empty before the write, the producer sends `DATA_READY` to
   the consumer queue with zero wait.
5. The consumer wakes on the queue, handles the message, then calls
   `sensor_log_read()` for up to 16 records at a time. It releases the mutex
   before printing anything.

## Design notes

**Data and control are kept apart.** Samples live in the log, where a slow
consumer only means a longer backlog. The queue carries small control
messages. If samples went through the queue, a consumer stall would fill it
within 1.6 s and push out UI commands, and each queue send copies the data a
second time. One message per sample would do the same. That's why the
producer rings only on the empty-to-non-empty edge: while the log holds data,
the consumer is either already draining it or deliberately paused.

**No lost wake-ups.** The edge test and the push happen under the same mutex
the consumer reads under. The consumer's drain loop only stops once a read
returns fewer records than it asked for, which means the log was empty at that
moment. The next write therefore sees an empty log and rings. Two safety nets
cover the rest:
- If the queue is full when the producer rings, it retries on the next sample.
- The consumer never blocks on its queue for more than 250 ms, so the log gets
  drained even if a doorbell were lost.

**The ring buffer.** It overwrites the oldest record instead of rejecting new
ones. The producer must never wait for space, and when something has to go,
the newest data is the more valuable. Head and tail are free-running 32-bit
counters rather than indexes:
- fill = `head - tail`, so full and empty are never ambiguous and no slot is wasted;
- the array index is `counter & (capacity - 1)`, which is why the capacity is a power of two;
- the arithmetic stays correct across the 2^32 wrap (tested).

The module has no locking of its own. The locking policy belongs to
`sensor_log`, and keeping it out makes the buffer testable on a PC.

**Why it's in DDR, and how you can tell.** On the Zynq the default linker
script places all of `.bss` in `ps7_ddr_0`. The storage array is named
`.bss.sensor_log`, so the stock script still collects it into `.bss` (zeroed
at startup, no extra linker work), but it keeps its own entry in the `.map`
file. At runtime `sensor_log_init()` checks the address against the DDR window
and refuses to start if a modified linker script moved it into OCM. The
banner prints where it actually landed. Static storage rather than heap: the
size is fixed at link time, there's no fragmentation, and it's visible in the
map.

**Mutex rules.**
- Every ring buffer access goes through `sensor_log`, and therefore through the mutex.
- The lock is held only while records are copied in or out. The consumer
  copies a batch of up to 16 records into a local array, releases the lock,
  and only then formats and prints. Printing a line takes about 7 ms at
  115200 baud, and holding the lock across that would block the producer.
- Every take has a bounded wait. The producer's is 20 ms (a build-time check
  keeps it well inside the 100 ms period), so a reader that misbehaves costs
  samples, never timing. The consumer waits up to 100 ms and simply tries
  again on its next pass.

**Priority inheritance.** `xSemaphoreCreateMutex()` gives a mutex with
priority inheritance, which a binary semaphore doesn't have. If the producer
(prio 4) blocks on the mutex while the consumer (prio 1) holds it, the
consumer is raised to priority 4 until it gives the mutex back. Without that,
the UI task (prio 2) could pre-empt the consumer in the middle of its hold and
stretch the producer's wait: classic priority inversion. The lock hold time is
measured and shown in `d`.

**Why not a critical section any more.** Stage 2 masked interrupts to copy
one 32-byte record. Copying batches of 16 records with interrupts masked
would add directly to interrupt latency, including the sample timer's. A
mutex costs very little when uncontended, doesn't touch interrupts, and makes
contention measurable. The producer statistics are still copied under a
critical section: a single fixed-size copy, and it keeps the stats path
independent of the log.

**Queue instead of notification bits.** Messages don't merge: pressing `x`
five times gives five "unknown command" lines. They carry a payload (the key),
and the queue depth is observable. The queue is bounded (16), and neither
sender ever blocks on it:
- The producer retries its doorbell on the next sample.
- The UI drops the key press and counts it, because blocking would stall the
  button scan and the UART RX drain.

**Console gatekeeper.** The stage 2 notes suggested a console mutex for stage
3. It isn't needed. Every task that wants something printed sends the
consumer a message, so the UART and the shared format buffer keep exactly one
owner. That's the FreeRTOS "gatekeeper task" pattern, and it's cheaper and
harder to get wrong than a lock around printf.

**Backlog handling.** After a pause (or a slow terminal), the consumer drains
at most 4 batches (64 records) per pass. It then glances at its queue with
zero wait and goes straight back to draining. A 4000-record backlog therefore
can't make `p` or `d` unresponsive. Timestamps in the stream are capture
times, so a drained backlog still shows the original 100 ms spacing.

**Where lost samples go.** `seq` increments for every sample the producer
takes, including one it then fails to store. Once the backlog is drained, the
counts in `d` must satisfy:

```
consumer lost  ==  log overwritten  +  producer drops (lock timeout)
```

If they don't, something is corrupting the buffer - which is exactly the
failure this stage exists to prevent.

## FreeRTOS BSP settings

Everything from stage 2, plus:

| FreeRTOSConfig.h | Value | Why |
|------------------|-------|-----|
| `configUSE_MUTEXES` | 1 (Xilinx default) | log mutex (*checked*, the build fails otherwise) |
| `configSUPPORT_DYNAMIC_ALLOCATION` | 1 (default) | queue and mutex are created at startup |
| `configQUEUE_REGISTRY_SIZE` | > 0 (optional) | the mutex and queue show up by name in kernel-aware debug |

Heap use grows only by the queue (16 x 8 bytes + control block) and the mutex
control block, all allocated before the scheduler starts. The 128 KiB log is
static and doesn't come from the FreeRTOS heap.

## Source layout

```
src/
  main.c                  peripheral + log init, task creation, scheduler start
  board/board_zybo.h      instance IDs, pins, TTC, DDR window              [DDR window new]
  drivers/                uart_drv, gpio_drv, xadc_drv, sample_timer     (unchanged)
  app/
    app_config.h          rates, priorities, stacks, log and queue sizing
    sensor_record.h       the 32-byte sample record                       [new]
    ring_buffer.*         overwrite-oldest FIFO, no locking                [new]
    sensor_log.*          DDR storage + ring buffer + mutex                [new]
    task_producer.*       timer-paced sampling, log writes, doorbell
    task_consumer.*       queue, log draining, console gatekeeper
    task_ui.*             buttons and keys -> queue messages
    fault.*, rtos_hooks.c, uptime.*, console.*                            (as stage 2)
tests/
  host/test_ring_buffer.c unit tests for ring_buffer.c, runs on a PC       [new]
```

## Unit tests (host)

`ring_buffer.c` has no RTOS or Xilinx dependencies, so it's tested with an
ordinary PC compiler:

```
cd sw/stage3_ringbuffer_sync/tests/host
gcc -std=c11 -Wall -Wextra -Werror -I../../src/app -I../../src/drivers test_ring_buffer.c ../../src/app/ring_buffer.c -o test_ring_buffer
./test_ring_buffer
```

What's covered:
- argument checks (NULL, zero and non-power-of-two capacity)
- empty buffer, FIFO order, partial pops
- overwrite-oldest when full, and the overwrite count
- peek-newest, including after everything has been popped
- the free-running counters crossing the 2^32 wrap
- 200 000 random push/pop steps compared against a naive reference model

Current result: `722811 checks, 0 failed`. To confirm the tests have teeth, a
deliberately broken buffer (tail not advanced on overwrite) makes them fail
within the first 100 random steps.

## Running on the board (Vivado / Vitis 2025.2)

Written against 2025.2, which only has the Unified Vitis IDE (SDT flow).
Older versions work too, but menu names differ.

### 1. Hardware platform in Vivado

Either run `hw/scripts/create_ps_platform.tcl` (see `hw/Readme.md`), or build it by hand:

1. **Create Project** > name `zybo_ps_platform`, location `<repo>/hw/build` > **RTL Project**,
   tick *Do not specify sources at this time*.
2. Default Part > **Boards** tab > search `zybo` > pick your board (Zybo Z7-10, Z7-20 or the original Zybo) > Finish.
3. Flow Navigator > **Create Block Design** > name `ps_system`.
4. **+** (Add IP) > `ZYNQ7 Processing System`.
5. Click **Run Block Automation** in the green banner > keep *Apply Board Preset* ticked > OK.
6. Double-click the Zynq block:
   - *PS-PL Configuration* > AXI Non Secure Enablement > GP Master AXI Interface > **untick M AXI GP0 interface**
   - *MIO Configuration* > Application Processor Unit > **tick Timer 0** (IO: EMIO)
   - check only, don't change: *I/O Peripherals* has UART 1 on MIO 48..49 and GPIO MIO ticked
   - OK
7. **Validate Design** (F6). It should report no errors.
8. Sources > right-click `ps_system.bd` > **Create HDL Wrapper** > let Vivado manage it.
9. Right-click `ps_system.bd` > **Generate Output Products** > Global > Generate.
10. File > Export > **Export Hardware** > **Pre-synthesis** > save as
    `<repo>/hw/export/zybo_ps_platform.xsa`. There is no bitstream: the PL is empty.

### 2. Platform in Vitis

1. Start Vitis 2025.2 and open a workspace folder, e.g. `<repo>/sw/workspace` (git-ignored).
2. File > New Component > **Platform** > name `zybo_platform` > select the XSA >
   Operating system **freertos**, processor **ps7_cortexa9_0** > Finish.
3. Open the platform's settings (`vitis-comp.json`) > ps7_cortexa9_0 > freertos domain > Board Support Package:
   - **freertos**: the defaults are fine. Check `freertos_use_mutexes` is on and
     `freertos_check_for_stack_overflow` is 2.
   - **xiltimer**: `XILTIMER_tick_timer` must **not** be `ps7_ttc_2` (the sample
     timer). The default and the SCU timer are both fine.
4. Select the platform in the FLOW panel > **Build**.

### 3. Application in Vitis

1. File > New Component > **Application** > name `sensor_app` > platform `zybo_platform` >
   domain `freertos_ps7_cortexa9_0` > Finish.
2. Copy every `.c` and `.h` from this stage's `src/` tree **flat** into the component's
   `src/` folder. The Vitis app template only compiles sources directly in `src/`,
   not in subfolders, and the quoted includes then resolve without any include
   path settings:
   ```powershell
   Get-ChildItem "<repo>\sw\stage3_ringbuffer_sync\src" -Recurse -Include *.c,*.h |
       Copy-Item -Destination "<workspace>\sensor_app\src" -Force
   ```
   Don't copy `tests/`.
3. Select `sensor_app` > **Build**. It should finish without errors or warnings from these sources.

### 4. Board and run

1. Boot mode jumper **JP5 to JTAG**. Power select to USB (or plug in a 5 V supply).
2. Micro-USB into the **PROG/UART** port, power switch on. Windows shows a
   "USB Serial Port (COMx)" in Device Manager.
3. Terminal on that COM port, **115200 8N1, no flow control** (Vitis > Serial
   Monitor, PuTTY or Tera Term).
4. Select `sensor_app` > **Run**. Vitis resets the board, runs ps7_init and
   downloads the ELF. The banner should appear, then the checklist below applies.

When something needs changing during bring-up, change it in the repository
and copy it over again, so the repo stays the source of truth.

In 2025.2 the Xilinx FreeRTOSConfig.h doesn't enable
`INCLUDE_uxTaskGetStackHighWaterMark`, so the `stack headroom` line in `d` is
left out. That's expected.

## Terminal interface

| Key | Button | Action |
|-----|--------|--------|
| `r` | BTN4 | full report: newest sample in the log + min/max over all samples |
| `s` | BTN5 | start/stop the telemetry stream (on at boot) |
| `f` | | stream rate: every sample (10 Hz) / every 10th (1 Hz) |
| `p` | | pause/resume reading the log - samples keep accumulating in DDR |
| `t` | | print the newest sample once (works while paused) |
| `d` | | diagnostics |
| `h` / `?` | | help |
| other | | `unknown command 'x'` - the key travels in the queue message |

Startup (the address depends on the link):

```
========================================================
 Zybo sensor aggregator - stage 3, log + mutex + queue
 fw 0.3.0, built Sep 12 2026 14:02:51
========================================================
XADC up: continuous sequencer, 16x averaging, calibration on
sensor log: 4096 records, 131072 bytes at 0x<addr> (DDR)
tasks created, starting scheduler
scheduler running, sample period 99998 us
```

Pause and resume - the backlog comes out in a burst, with the original timestamps and no gaps:

```
[    20.118] #196    T 44.91 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
log reading PAUSED - samples accumulate in the log
                                              ... 30 s pass, nothing printed, LD4 holds ...
log reading RESUMED
[    20.218] #197    T 44.91 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
[    20.318] #198    T 45.03 C | VCCINT 1.001 V | VCCPINT 1.000 V | BTN4 0 BTN5 0
...
```

Diagnostics layout (fill in the real numbers during bring-up):

```
Diagnostics
  uptime          : <s>
  sample period   : 99998 us (timer)
  producer        : <n> samples, 0 overruns, 0 timeouts
  sample interval : min <n> us, max <n> us
  XADC read time  : max <n> us
  log write       : max <n> us incl. mutex wait, 0 dropped on lock timeout
  sensor log      : <n> / 4096 records waiting, peak <n>, 0 overwritten
  log mutex       : held max <n> us, 0 consumer lock timeouts
  log storage     : 131072 bytes at 0x<addr> (DDR)
  consumer        : <n> received, 0 lost, stream on, every sample, draining on
  message queue   : peak <n> / 16, 0 UI messages dropped, 0 doorbell retries
  UART rx errors  : overrun 0, framing 0, parity 0
  stack headroom  : producer <n>, ui <n>, consumer <n> words
  heap free       : <n> bytes
```

## Bring-up checklist

Basic operation
- [ ] Banner shows the log address inside DDR; the `.map` file shows `.bss.sensor_log` at the same address
- [ ] Stream as in stage 2: 100 ms steps, no sequence gaps, LD4 at 1 Hz
- [ ] `d`: lost 0, overwritten 0, drops 0; log peak stays at 1-2 records; overruns 0
- [ ] `d`: note the mutex hold max and log write max - both should be well under a millisecond

Ring buffer
- [ ] `p`, wait 30 s, `d`: records waiting ~300 and climbing by 10 a second; `r` and `t` still show fresh samples
- [ ] `p` again: the backlog streams out with its original timestamps, no gaps; `d`: lost 0, peak ~300
- [ ] Overflow: set `APP_LOG_CAPACITY` to 256, pause for 60 s, resume. Expected: one jump in the sequence numbers,
      `d` shows overwritten ~350 and lost == overwritten. Restore 4096 afterwards

Mutex under contention (`APP_TEST_LOG_HOLD_US`, revert afterwards)
- [ ] 15000 (15 ms) plus a backlog from `p`: log write max rises towards 15 ms, drops stay 0,
      sample interval and overruns unchanged
- [ ] 50000 (50 ms) plus a backlog: drops appear (the producer gives up after 20 ms), the stream shows gaps,
      `lost == overwritten + drops` still holds, sample interval and overruns **still** unchanged -
      a misbehaving reader costs samples, never timing

Queue
- [ ] Press `x` five times quickly: five `unknown command 'x'` lines (no merging as in stage 2)
- [ ] Paste 40 `d` characters at once: queue peak reaches 16, `d` shows UI messages dropped, nothing hangs

Robustness carried over from stage 2
- [ ] Dead timer test (`sample_timer_stop()` at seq 50): WARN after 1 s, even with draining paused
- [ ] Stack headroom after a long run with a big backlog drain - the consumer's batch buffer is 512 bytes of stack

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `#error sensor_log needs configUSE_MUTEXES = 1` | enable mutexes in the FreeRTOS BSP settings |
| Build error on `XPAR_XTTCPS_2_*` | TTC0 not enabled in the XSA - tick Timer 0 in the Zynq block, re-export, update the platform |
| Undefined references to functions in `app/` or `drivers/` files | sources were copied into subfolders - the Vitis app template only builds `src/*.c`, copy them flat |
| `FATAL: sample timer init failed, code 2` | TTC instance not found - same XSA problem as above |
| `FATAL: sample timer init failed, code 6` | counter already running - xiltimer tick_timer is set to `ps7_ttc_2`, change it |
| Nothing after "scheduler running", LD4 not blinking, WARN line after 1 s | timer interrupt not arriving - check the XSA has TTC0 and the platform was rebuilt |
| `FATAL: sensor log init failed, code 1` | log storage outside DDR - the linker script was changed to put .bss elsewhere |
| `FATAL: sensor log init failed, code 3` | no heap left for the mutex - increase the BSP heap size |
| `FATAL: consumer task/queue create failed` | no heap left for the queue or the task |
| `lost` doesn't match `overwritten + drops` after draining | buffer corruption - check for code touching the ring outside `sensor_log` |
| Backlog never drains after `p` | draining still paused (`d` shows PAUSED), or consumer lock timeouts climbing |
| Build error on the `sensor_record_t size` assertion | the record layout changed - revisit `APP_LOG_CAPACITY` and this Readme's sizing |

## Known limitations (on purpose, handled in later stages)

- Nothing notices a task that hangs. If the consumer got stuck while holding
  the mutex, the producer would drop every sample from then on, and the board
  would never recover. Stage 4: the hardware watchdog and task check-ins.
- The log lives in RAM and is lost on reset.
- UART TX is still polled, so the consumer busy-waits on the FIFO at the lowest priority.

## Next: stage 4

The Zynq system watchdog (SWDT) and a dedicated task that only kicks it when
every other task has checked in recently. A hang anywhere means no kick, and
the board resets.
