# Session 4 — post-reboot retest plan

This file was written by Claude at the end of session 4, just before a
reboot. Read it after rebooting and follow the steps in order.

## Context (one paragraph)

Session 4 added four defensive changes to the M3 step-1 iso URB engine
(no-resubmit-on-error, 1-URB throttle, interval=8, USB_SPEED_HIGH check)
to prevent the hard freeze documented in `CLAUDE.md` fact #17. We never
got to test them: the next insmod/rmmod cycle oopsed in
`snd_pcm_dev_disconnect` with `list_del corruption` (fact #18 in
`CLAUDE.md`) before aplay was even attempted. The pre-reboot dmesg is
in `session4-rmmod-oops.txt`. Top suspect for that crash is PipeWire's
rapid open/hw_params/hw_free/close cycles racing our disconnect path,
since hw_params now does real work (URB alloc/free).

The retest below eliminates that variable by stopping PipeWire entirely
before any kernel module operation.

## Step 1 — stop PipeWire / WirePlumber

```bash
systemctl --user stop pipewire pipewire-pulse wireplumber
systemctl --user stop pipewire.socket pipewire-pulse.socket
pgrep -a 'pipewire|wireplumber'   # should print nothing
```

If sockets re-activate on demand (you'll see processes come back after
a few seconds), mask them temporarily:

```bash
systemctl --user mask pipewire pipewire-pulse wireplumber \
                       pipewire.socket pipewire-pulse.socket
```

Revert after the session with `systemctl --user unmask <same>`.

## Step 2 — stream dmesg to disk BEFORE any insmod

In a dedicated terminal, leave running for the whole session:

```bash
sudo dmesg -w | tee -a /tmp/mc707-session4-take2.log
```

Do **not** `sudo dmesg -C` between insmod and rmmod. If anything goes
wrong, the file on disk is the only artifact that will survive.

## Step 3 — the test

In another terminal:

```bash
sudo insmod /home/xecaz/code/snd-roland-mc707/snd-roland-mc707.ko
sleep 2                                  # let probes settle
cat /proc/asound/cards                   # confirm hw:1 = MC707
aplay -D plughw:1,0 -f S24_LE -r 44100 -c 6 /dev/zero
# Ctrl-C after ~3 seconds
sudo rmmod snd_roland_mc707
lsmod | grep mc707                       # check for refcount -1 zombie
```

## Step 4 — interpret the outcome and report back

| Outcome | What it means | What to do next |
|---|---|---|
| rmmod clean (no oops, no `-1` zombie); aplay log shows `activate_alt` and `iso URB submit` lines | PipeWire was the racer (fact #18 confirmed). Defensive iso changes held. Proceed with M3 testing under PipeWire-masked conditions; fix disconnect-lock pattern later. | Report the aplay-window log entries (probe → activate_alt → trigger → URB completion). |
| rmmod still oopses with similar `list_del` corruption | Bug is in our disconnect logic, independent of PipeWire. The lock-across-`snd_card_disconnect` pattern in `mc707_disconnect` needs fixing first. | Report the full dmesg from `/tmp/mc707-session4-take2.log`. |
| Hard freeze during aplay (fact #17 recurs) | Defensive M3 changes weren't enough. Streamed log will tell us how far we got and what the device returned. | Power-cycle, then report whatever is on disk in `/tmp/mc707-session4-take2.log`. |
| aplay errors but kernel stays stable, rmmod clean | Best possible signal. We've isolated iso submission from kernel teardown. | Report the entries from t=insmod to t=rmmod. |

## Notes

- Don't restore the 4-URB ring or `urb->interval = 1` in the code; those
  changes are still locked behind "iso submission proven safe."
- Pacing math is still wrong by design (5/6 frames-per-packet at 1ms
  cadence underfills the EP by 8×). That's deliberate — we want silence
  bytes on the wire to observe device behavior, not playable audio. Fix
  pacing for M3 step 2.
- If PipeWire being stopped breaks the rest of your audio (HDA Intel
  card), you'll lose system sound for the duration of the test. That's
  expected and reversible.
