# Stage 4 - Hardware watchdog

Status: **code complete, builds and links against the Vitis 2025.2 BSP. Not run on the board yet.**

Last firmware stage. The Zynq system watchdog (SWDT) resets the board when a
task stops making progress. One supervisor task is the only thing that kicks
it, and it only does so while every other task keeps checking in.

Everything from stage 3 is unchanged apart from the check-in calls, so the
stage 3 Readme is still the reference for the log, mutex and queue design.

## What's new

- `drivers/wdt_drv` - SWDT: timeout, reset output, start, kick
- `drivers/slcr` - small SLCR read/modify helper (`gpio_drv` uses it now too)
- `system/reset_cause` - reads and clears the reset reason at boot; shown in the banner and in `d`
- `tasks/task_watchdog` - the supervisor and the check-in API
- producer, ui and consumer check in once per loop
- keys `1`-`4` hang things on purpose, for testing
- `fault_halt()` ends in a watchdog reset once the SWDT is running, instead of halting forever
- `boot/boot.bif` for an SD card boot image
- hw script enables the SWDT

## How it works

| Task | Checks in | Limit |
|------|-----------|-------|
| producer | after every stored sample (every 100 ms) | 500 ms |
| ui | every scan (every 10 ms) | 500 ms |
| consumer | every loop pass (at least every 250 ms, longer while printing) | 2000 ms |

The supervisor runs at the highest application priority every 250 ms. It
looks at how long ago each task last checked in. If all are within their
limit it kicks the SWDT. If one isn't, it prints which task and stops kicking
for good. The SWDT timeout is 2 s, so from a hang to the reset takes at most
limit + 250 ms + 2 s.

Why check-ins and not just a kick from the lowest priority task: that only
catches a task spinning in a loop and starving everything below it. A task
blocked forever (a missed wake-up, a deadlock) doesn't stop anything else from
running, so it would go unnoticed. Check-ins catch both. If the kernel stops
entirely (interrupts off, scheduler gone), the supervisor stops with it and
the SWDT resets on its own.

A few details:
- The producer only checks in after a sample actually got stored, so a dead
  sample timer ends in a reset as well.
- No second chance: a task that was late and then recovers still gets the board reset.
- The SWDT is started in `main()` just before `vTaskStartScheduler()`. If the
  scheduler never runs, nothing kicks.
- The SWDT keeps counting while a debugger has the CPU halted. Set
  `APP_WDT_ENABLE` to 0 in `app_config.h` before debugging with breakpoints.
- The reset reason bits in `REBOOT_STATUS` are sticky, so they're cleared
  after reading. Otherwise the next boot would still report the old watchdog reset.

## Hardware change

The SWDT has to be enabled in the XSA:

- Vivado: double-click the Zynq block > *MIO Configuration* > *Application
  Processor Unit* > tick **Watchdog** (leave IO on EMIO) > OK, then Generate
  Output Products and Export Hardware again. Or re-run
  `hw/scripts/create_ps_platform.tcl`, which now does it.
- Vitis: point the platform at the new XSA (in the platform's settings,
  *Switch XSA* / update hardware specification) and rebuild the platform.
  Recreating the platform component works just as well.

If the application build complains about `XPAR_XWDTPS_0_*`, the platform is
still built from the old XSA.

## Build and run

Same steps as in the stage 3 Readme, just with this stage's sources. From a
Command Prompt:

```bat
for /R "D:\ANEK_GIT\zybo-rtos-sensor-aggregator\sw\stage4_watchdog\src" %f in (*.c *.h) do copy /Y "%f" "D:\ANEK_GIT\zybo-rtos-sensor-aggregator\sw\workspace\sensor_app\src\"
```

That overwrites the stage 3 files in `sensor_app/src` and adds the new ones.
Then Build and Run.

### Testing a real reset: boot from SD

Over JTAG the watchdog reset does happen, but afterwards the BootROM waits
for the debugger again (JP5 is on JTAG), so the firmware never comes back.
You'd just see the output stop and Vitis lose the session. To see the whole
cycle - hang, reset, reboot, `last reset: WATCHDOG (SWDT)` - boot from SD:

1. Build the application. The platform build already produced `fsbl.elf`.
2. Create `BOOT.BIN`. The paths in `boot.bif` are relative to its own folder,
   so run bootgen from there:
   ```bat
   cd /d D:\ANEK_GIT\zybo-rtos-sensor-aggregator\sw\stage4_watchdog\boot
   C:\AMDDesignTools\2025.2\Vitis\bin\bootgen.bat -image boot.bif -arch zynq -o BOOT.BIN -w on
   ```
   (Vitis > Create Boot Image does the same with the same two files.)
3. Copy `BOOT.BIN` to the root of a FAT32 microSD card, move JP5 to **SD**, power cycle.

## Terminal

As stage 3, plus:

| Key | Test | Expected |
|-----|------|----------|
| `1` | producer spins in a busy loop | `WATCHDOG: producer ...`, stream stops, reset after ~2.5 s |
| `2` | ui task blocked forever | `WATCHDOG: ui ...`, stream keeps running until the reset |
| `3` | consumer spins in a busy loop | stream stops at once, `WATCHDOG: consumer ...` after ~2 s, then the reset |
| `4` | interrupts off | nothing more is printed, reset about 2 s later |

`d` gets two more lines:

```
  last reset      : power-on
  watchdog        : SWDT 2000 ms, <n> kicks, worst silence producer <n> / ui <n> / consumer <n> ms
```

## Bring-up checklist

Left over from stage 3 (run these first):
- [ ] Timestamps step by 100 ms, sequence numbers without gaps
- [ ] BTN4/BTN5 read 0 released and 1 while held, BTN4 gives one report per press -
      this confirms the MIO button wiring assumption in `board_zybo.h`
- [ ] `p` for 30 s and back: backlog comes out with no gaps, `d` shows lost 0

Watchdog:
- [ ] Banner shows `last reset: power-on` after power-up, then `watchdog: SWDT 2000 ms, supervisor every 250 ms`
- [ ] `d` after a few minutes: kicks go up by about 4 a second; note the worst silence per task -
      the limits in `app_config.h` should be several times those numbers
- [ ] From SD: `1`, `2`, `3` and `4` each end in a reset, and the next banner says `last reset: WATCHDOG (SWDT)`
- [ ] Power cycle after that: `last reset: power-on` again (flags were cleared)
- [ ] `r` and `d` spammed during a backlog drain don't trip the consumer limit
- [ ] With `APP_WDT_ENABLE 0`: banner warns, `1` still prints the WATCHDOG line but no reset follows

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| `XPAR_XWDTPS_0_*` undeclared | SWDT not in the XSA, or the platform wasn't updated |
| `FATAL: watchdog init failed, code 1` | same - the SWDT instance wasn't found |
| Board resets while stopped at a breakpoint | the SWDT counts on while the CPU is halted - build with `APP_WDT_ENABLE 0` |
| After a watchdog reset nothing comes back | booting over JTAG, see *Testing a real reset* |
| `last reset: debugger` or `software (SLCR)` on every JTAG run | expected, Vitis resets the board before downloading |
| Resets every few seconds right after boot | some task never checks in - read the `WATCHDOG:` line printed just before the reset |

## Known limitations

- The MIO button wiring on the Z7 is still unconfirmed (first checklist item).
- The sensor log lives in DDR and doesn't survive the reset; only the reset cause does.
- Test keys are compiled in by default. Set `APP_WDT_TEST_COMMANDS` to 0 for anything that isn't a bench build.
- No count of watchdog resets is kept across resets (TODO in `reset_cause.c`).
- The stack headroom line in `d` doesn't include the supervisor task - it isn't shown on 2025.2 anyway.

## What's left

The firmware side is done. The PL design (AXI GPIO for SW0-3 and BTN0-3) is next, on the hardware side.
