# Stage 4 - Hardware watchdog

**Status:** code complete. Built end to end with Vivado and Vitis 2025.2 - XSA
from the script, FreeRTOS platform, application - with no warnings from these
sources. Not run on the board yet.

This is the last firmware stage. On top of everything from stage 3, the
Zynq's system watchdog (SWDT) now resets the board when a task stops making
progress. A small supervisor task is the only code allowed to kick it, and it
only does that while the other three tasks keep checking in.

The sensor log, mutex and queue haven't changed apart from the check-in calls,
so for that side of things the stage 3 Readme is still the reference.

## What's new compared to stage 3

- **Watchdog driver** (`drivers/wdt_drv`): sets the SWDT timeout, turns the reset output on, starts and kicks it.
- **Supervisor task** (`tasks/task_watchdog`): the highest priority task. It
  kicks the SWDT every 250 ms, but only while every monitored task has checked in recently.
- **Check-ins**: the producer, ui and consumer tasks each call `task_watchdog_checkin()` once per loop.
- **Reset cause** (`system/reset_cause`): read at boot and cleared, shown in the banner and in `d`.
- **SLCR helper** (`drivers/slcr`): unlock / modify / lock in one place. `gpio_drv` uses it now instead of its own copy.
- **Test keys** `1` to `4`: hang a task on purpose, to watch the watchdog do its job.
- **Faults end in a reset**: `fault_halt()` still prints and blinks LD4, but with the SWDT running the board resets a couple of seconds later instead of staying halted.
- **SD boot image** (`boot/boot.bif`): for testing a real reset-and-reboot cycle.
- **Hardware**: the SWDT is enabled in the XSA (hw script and manual steps below).

## How the watchdog works

### The supervisor and the check-ins

| Task | Checks in | Limit |
|------|-----------|-------|
| producer | after every sample it stores (every 100 ms) | 500 ms |
| ui | every button scan (every 10 ms) | 500 ms |
| consumer | every pass of its loop (at least every 250 ms, longer while it prints) | 2000 ms |

Every 250 ms the supervisor looks at when each task last checked in. If all
three are within their limit, it kicks the SWDT. If one isn't, it prints a
line naming the task and stops kicking for good. With a 2 s SWDT timeout the
board resets shortly after.

Why check-ins, rather than just kicking from the lowest priority task? A kick
from the bottom only notices a task that spins in a loop and starves
everything below it. A task that's blocked forever - a missed wake-up, a
deadlock - doesn't stop anyone else from running, so nothing would notice.
Check-ins catch both. And if the kernel itself stops (interrupts masked,
scheduler gone), the supervisor stops with it and the SWDT resets the board
on its own.

### Timing

Worst case from a hang to the reset is *limit + 250 ms + 2 s*:

| What hangs | Watchdog line printed after | Board resets after |
|------------|----------------------------|--------------------|
| producer or ui | 0.5 - 0.75 s | about 2.5 s |
| consumer | 2 - 2.25 s | about 4 s |
| the kernel (no interrupts) | nothing is printed | at most 2 s |

### Details worth knowing

- The producer only checks in after a sample was actually stored. So a
  sample timer that stops firing ends in a reset as well - not just a stuck task.
- A sample dropped on a mutex timeout still counts as progress. That's a data
  loss, counted in `d`, not a hang.
- There's no second chance. Once a task was late, the kicks stay off even if
  the task recovers a moment later.
- The SWDT is started in `main()` right before `vTaskStartScheduler()`. If the
  scheduler never gets going, nothing kicks and the board resets.
- The SWDT keeps counting while a debugger has the CPU stopped. Stepping
  through code with it running ends in a reset after 2 s. Set `APP_WDT_ENABLE`
  to 0 for debugging.
- The FSBL (SD boot) runs the watchdog itself during boot and stops it before
  starting the application. `wdt_drv_init()` stops it again anyway, then sets
  its own timeout.
- The timeout comes from the watchdog clock in the generated BSP: 111.1 MHz on
  the Z7, giving 2000 ms. Vivado's PS7 settings show a "133.33" for the
  watchdog, but that's not what the BSP uses.

### Reset cause

The SLCR keeps a flag for every kind of reset in `REBOOT_STATUS`. The flags
survive everything except a power cycle, so `reset_cause_capture()` reads them
first thing in `main()` and then clears them. Otherwise an old watchdog reset
would keep showing up.

| Banner / `d` shows | Meaning |
|--------------------|---------|
| `power-on` | the board was powered up |
| `WATCHDOG (SWDT)` | the watchdog reset the board |
| `CPU private watchdog` | the Cortex-A9 watchdog - not used by this firmware |
| `PS reset button` | the PS-SRST button |
| `debugger` / `software (SLCR)` | reset through JTAG or the SLCR - what a Vitis run over JTAG looks like |
| `unknown` | no flag at all - the program was restarted without any reset, e.g. from the debugger |

The FSBL only clears the watchdog flag if the watchdog fired while the FSBL
itself was still running. A reset caused by this application is still there
when it boots again, which is what makes the SD card test below work.

## Files in this stage

```
src/
  main.c                    reset cause, init, SWDT start, scheduler
  config/app_config.h       + watchdog settings and supervisor priority/stack
  config/board_zybo.h       + SWDT instance
  drivers/wdt_drv.*         SWDT                                            [new]
  drivers/slcr.*            SLCR read / modify with unlock                  [new]
  drivers/gpio_drv.c        uses slcr.h for the button pull-ups
  system/reset_cause.*      reset reason from REBOOT_STATUS                 [new]
  system/console.*          + console_write_dec(), no printf needed
  system/fault.*            ends in a watchdog reset when the SWDT runs
  tasks/task_watchdog.*     supervisor, check-ins, hang tests               [new]
  tasks/task_producer.c     check-in after each stored sample
  tasks/task_ui.c           check-in per scan, keys 1-4
  tasks/task_consumer.*     check-in per pass, test commands, watchdog lines in 'd'
  (everything else unchanged from stage 3)
tests/host/                 ring buffer unit tests, as stage 3
boot/boot.bif               SD card boot image description                  [new]
```

## Hardware: enabling the watchdog

The SWDT has to be in the XSA, otherwise the build stops at `XPAR_XWDTPS_0_*`.

### Carrying on from the stage 3 Vivado project

This assumes the project from the stage 3 walkthrough
(`hw/build/zybo_ps_platform.xpr`, block design `ps_system`, Zybo Z7-20).

1. Vivado 2025.2 > **Open Project** > `hw/build/zybo_ps_platform.xpr`.
2. Flow Navigator > IP INTEGRATOR > **Open Block Design**.
3. Double-click the **ZYNQ7 Processing System** block.
4. **MIO Configuration** page, expand **Application Processor Unit**:
   - tick **Watchdog**, IO column **EMIO**
   - check **Timer 0** is still ticked (EMIO)
5. Same page, expand **I/O Peripherals > GPIO > GPIO MIO**. Find the rows for
   **MIO 50** and **MIO 51** (listed as `gpio[50]` and `gpio[51]`) and set
   **Pullup** to **disabled** on both. (The firmware clears them at boot anyway,
   but the XSA should say what the board really needs.)
6. Optional, since nothing is in the PL: **Clock Configuration > PL Fabric
   Clocks**, untick **FCLK_CLK0**. On **PS-PL Configuration > General >
   Enable Clock Resets**, untick **FCLK_RESET0_N**.
7. **OK**, then **Validate Design** (F6). It should come back with no errors.
8. Sources > right-click `ps_system.bd` > **Generate Output Products** > Global > **Generate**.
   The HDL wrapper updates by itself if it was created as "Let Vivado manage".
9. **File > Export > Export Hardware** > **Pre-synthesis** > file
   `hw/export/zybo_ps_platform.xsa` (overwrite the old one) > Finish.

If you ran synthesis or implementation earlier, those runs now show as out of
date. You don't need to re-run them - there's no bitstream to make.

### Starting fresh instead

From the `hw` folder:

```
vivado -mode batch -source scripts/create_ps_platform.tcl -tclargs zybo-z7-20
```

It creates the project under `hw/build/zybo_ps_platform/` and writes
`hw/export/zybo_ps_platform.xsa` with all of the above already set. The four
DDR critical warnings in its log come from the Digilent board preset and are
expected (see `hw/Readme.md`).

## Vitis: loading the new XSA

The platform in the workspace was built from the old XSA, so it needs updating.
Then rebuild the platform, and after it the application.

**In the IDE.** Open the platform component's settings (`vitis-comp.json`
under `zybo_platform`). Use the option there to switch or update the hardware
design (XSA), and pick the new `hw/export/zybo_ps_platform.xsa`. Then **Build**
`zybo_platform`, then **Build** `sensor_app`.

**With a script**, if you can't find that option. This one has been tried with
2025.2: it swaps the XSA and rebuilds both components. Close the Vitis IDE
first and save this as `update_platform.py`:

```python
import vitis

client = vitis.create_client()
client.set_workspace(r"D:\ANEK_GIT\zybo-rtos-sensor-aggregator\sw\workspace")

platform = client.get_component("zybo_platform")
platform.update_hw(r"D:\ANEK_GIT\zybo-rtos-sensor-aggregator\hw\export\zybo_ps_platform.xsa")
platform.build()

client.get_component("sensor_app").build()
vitis.dispose()
```

and run it from a Command Prompt:

```bat
C:\AMDDesignTools\2025.2\Vitis\bin\vitis.bat -s update_platform.py
```

**Or start the components over.** Delete `sensor_app` and `zybo_platform`, and
create them again from the new XSA exactly as in the stage 3 walkthrough. It
takes a few minutes longer, but there's nothing to go wrong.

To check it worked, look for `XPAR_XWDTPS_0_BASEADDR` in
`sw/workspace/zybo_platform/export/zybo_platform/sw/freertos_ps7_cortexa9_0/include/xparameters.h`.

## Building and running

Copy this stage's sources into the application flat, like before. From a Command Prompt:

```bat
for /R "D:\ANEK_GIT\zybo-rtos-sensor-aggregator\sw\stage4_watchdog\src" %f in (*.c *.h) do copy /Y "%f" "D:\ANEK_GIT\zybo-rtos-sensor-aggregator\sw\workspace\sensor_app\src\"
```

That overwrites the stage 3 files and adds the new ones. Build `sensor_app`,
then Run it over JTAG as before. The start of the output:

```
========================================================
 Zybo sensor aggregator - stage 4, watchdog
 fw 0.4.0, built <date> <time>
========================================================
last reset: power-on
XADC up: continuous sequencer, 16x averaging, calibration on
sensor log: 4096 records, 131072 bytes at 0x00130E10 (DDR)
watchdog: SWDT 2000 ms, supervisor every 250 ms
tasks created, starting scheduler
scheduler running, sample period 100000 us
```

(`last reset` will say `debugger` or `software (SLCR)` on later runs over JTAG,
and the log address can move a little between builds.)

### Seeing a real watchdog reset: boot from SD

Over JTAG the watchdog reset does happen. But afterwards the BootROM sits and
waits for the debugger (JP5 is on JTAG), so the firmware never comes back -
the output just stops and Vitis loses the connection. To see the whole cycle
(hang, reset, reboot, `last reset: WATCHDOG (SWDT)`), boot from the SD card:

1. Build the application. The platform build has already produced the FSBL.
2. Make `BOOT.BIN`. The paths in `boot.bif` are relative to its own folder
   and assume the workspace at `sw/workspace`, so run bootgen from there:
   ```bat
   cd /d D:\ANEK_GIT\zybo-rtos-sensor-aggregator\sw\stage4_watchdog\boot
   C:\AMDDesignTools\2025.2\Vitis\bin\bootgen.bat -image boot.bif -arch zynq -o BOOT.BIN -w on
   ```
   Vitis > Create Boot Image does the same thing if you give it the FSBL as
   the bootloader and `sensor_app.elf` after it.
3. Copy `BOOT.BIN` to the root of a FAT32 microSD card.
4. Move **JP5 to SD**, put the card in, power cycle. Keep the terminal open - the
   board prints the same banner as over JTAG.

Rebuild `BOOT.BIN` every time the application changes. `BOOT.BIN` is git-ignored.

## Terminal commands

| Key | Button | Action |
|-----|--------|--------|
| `r` | BTN4 | full report: newest sample + min/max over all samples |
| `s` | BTN5 | start/stop the telemetry stream |
| `f` | | stream rate: every sample (10 Hz) / every 10th (1 Hz) |
| `p` | | pause/resume reading the log |
| `t` | | print the newest sample once |
| `d` | | diagnostics |
| `h` / `?` | | help |
| `1` | | test: producer stuck in a busy loop |
| `2` | | test: ui task blocked forever |
| `3` | | test: consumer stuck in a busy loop |
| `4` | | test: kernel interrupts masked, the whole system stops |

What to expect from the tests (with the SWDT running):

| Key | On the terminal | Then |
|-----|-----------------|------|
| `1` | `TEST:` line, stream stops, `WATCHDOG: producer has not checked in for ...` | reset ~2.5 s after the key |
| `2` | `TEST:` line, stream keeps running, `WATCHDOG: ui ...` | reset ~2.5 s after the key |
| `3` | `TEST:` line, stream stops, `WATCHDOG: consumer ...` about 2 s later | reset ~4 s after the key |
| `4` | `TEST:` line, then nothing | reset within 2 s |

With `APP_WDT_ENABLE` 0 the tests still hang the task and still print the
`WATCHDOG:` line, but nothing resets the board - press PS-SRST or power cycle.

## Diagnostics (`d`)

Stage 3's lines, plus these at the end:

```
  heap free       : <n> bytes
  last reset      : power-on
  watchdog        : SWDT 2000 ms, <n> kicks
  worst silence   : producer <n> ms, ui <n> ms, consumer <n> ms
```

`kicks` goes up by about 4 a second. `worst silence` is the longest time the
supervisor has seen since a task's last check-in. It's measured in RTOS ticks
(10 ms at the default tick rate), and it's what to look at before touching the
limits. Each limit should stay several times bigger than its worst silence.

The `stack headroom` line from stage 2 (now including the supervisor) only
appears if `INCLUDE_uxTaskGetStackHighWaterMark` is on in the BSP, and it isn't by
default in 2025.2.

## Settings (`config/app_config.h`)

| Setting | Default | Notes |
|---------|---------|-------|
| `APP_WDT_ENABLE` | 1 | 0 while debugging with breakpoints |
| `APP_WDT_TIMEOUT_MS` | 2000 | rounded up to what the SWDT counter can do; the real value is printed at boot |
| `APP_WDT_CHECK_PERIOD_MS` | 250 | supervisor period; has to fit twice into the timeout (checked at build time) |
| `APP_WDT_LIMIT_PRODUCER_MS` | 500 | at least two supervisor periods (checked) |
| `APP_WDT_LIMIT_UI_MS` | 500 | at least two supervisor periods (checked) |
| `APP_WDT_LIMIT_CONSUMER_MS` | 2000 | has to cover its 250 ms queue poll plus a long print (checked) |
| `APP_WDT_TEST_COMMANDS` | 1 | keys 1-4; set to 0 for anything that isn't a bench build |
| `APP_PRIO_WATCHDOG` | idle + 5 | must stay above the producer (checked) |

## Bring-up checklist

Still open from stage 3 - worth doing first, with this build:
- [ ] Timestamps step by 100 ms, sequence numbers without gaps
- [ ] BTN4/BTN5 read 0 when released and 1 while held, BTN4 gives exactly one report per press.
      This confirms the MIO button wiring assumption in `board_zybo.h`.
- [ ] `p` for 30 s and back: backlog comes out with no gaps, `d` shows lost 0

Watchdog, over JTAG:
- [ ] Banner shows `last reset: power-on` on the first run after powering up, and `watchdog: SWDT 2000 ms, supervisor every 250 ms`
- [ ] `d` after a few minutes: kicks keep going up, note the worst silence per task
- [ ] Spam `r` and `d` during a backlog drain (after `p`): no watchdog line
- [ ] `2`: the `WATCHDOG: ui` line appears, the stream keeps running until the output stops at the reset

Watchdog, booting from SD:
- [ ] `1`, `2`, `3` and `4` each end in a reboot, and the next banner says `last reset: WATCHDOG (SWDT)`
- [ ] Power cycle after that: `last reset: power-on` again
- [ ] Break the sample timer on purpose (`sample_timer_stop()` after 50 samples in the producer): `WATCHDOG: producer` and a reboot

With `APP_WDT_ENABLE` 0:
- [ ] Banner warns that the watchdog is disabled, `1` prints the `WATCHDOG:` line, the board stays hung until reset by hand

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Build error on `XPAR_XWDTPS_0_*` | the SWDT isn't in the XSA, or the platform still uses the old one - see *Vitis: loading the new XSA* |
| `FATAL: watchdog init failed, code 1` | SWDT instance not found - same as above |
| `FATAL: watchdog init failed, code 2` | driver init failed, or the BSP reports a watchdog clock of 0 |
| Board resets while stopped at a breakpoint | the SWDT counts on while the CPU is halted - build with `APP_WDT_ENABLE` 0 |
| After a watchdog reset nothing comes back | booting over JTAG - see *Seeing a real watchdog reset* |
| `last reset: unknown` | the program was restarted without a reset (debugger) - nothing wrong |
| Resets every few seconds right after boot | a task never checks in - read the `WATCHDOG:` line printed just before each reset |
| `WATCHDOG: consumer` during heavy output | the consumer limit is too tight for what's being printed - compare with `worst silence` and raise `APP_WDT_LIMIT_CONSUMER_MS` |
| SD boot: nothing at all on the terminal | `BOOT.BIN` not at the card root, card not FAT32, or JP5 not on SD |

## Known limitations

- The MIO button wiring on the Z7 is still unconfirmed (first checklist item).
- The sensor log lives in DDR and doesn't survive the reset. Only the reset cause does.
- The number of watchdog resets isn't kept anywhere (there's a TODO for it in `reset_cause.c`).
- The test keys are compiled in by default - remember `APP_WDT_TEST_COMMANDS` 0 for a real build.
- The supervisor itself isn't watched by anything except the SWDT. That's the point of the SWDT, but it means a supervisor bug shows up only as a reset without a `WATCHDOG:` line.

## What's next

The firmware is done. Next is the PL side: AXI GPIO for the slide switches
SW0-3 and buttons BTN0-3, and the hardware platform that goes with it.
