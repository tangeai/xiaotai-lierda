# Lite AEC adapter regression

These tests compile the production `src/media/audio_device.c` with the real
SDK audio header. Only the SDK/RTOS boundary is replaced. They exercise the
adapter's ownership and PCM handling, not the closed AEC algorithm or acoustic
quality on a board.

Run from the repository root with a host C compiler:

```powershell
python examples/NT26FxDx_OpenKit/tirtc_lite/tests/run_audio_aec_test.py --cc gcc
python examples/NT26FxDx_OpenKit/tirtc_lite/tests/run_dev_playback_gain_test.py --cc gcc
```

Both runners also accept `--cc "zig cc"` or an absolute compiler/wrapper path.
The AEC runner accepts repeatable `--scenario` arguments for a focused check.
Every scenario runs in a fresh process, with Group both enabled and disabled.
The existing gain runner explicitly sets `DEMO_AI_AUDIO_AEC_ENABLE=0`; the AEC
runner sets it to `1` and enables the host-only worker-step hook.

The fake clock advances without real sleeping. A registered background task
is serviced through `demo_ai_audio_aec_service_for_test`, rather than running
its infinite RTOS loop. The first recording callback models the driver's
asynchronous readiness. Subsequent recording callbacks reuse their buffer
immediately after return and disallow sleeping/RTOS critical APIs inside the
callback, matching the ISR context found in the audited D driver.
The vendor fake requires RecordStreamStop to quiesce RX/VEM before
PlayStreamStop, matching the SDK's internal DeInit order. This catches the
former playback-first path that could dismantle shared I2S with RX still live.
RecordStream then rebuilds capture; repeated RecordStream alone cannot repair
a dismantled path whose VEM flag was never cleared. Each rebuild
writes a simulated default codec level before the first callback, so the test
checks that the requested volume is restored after asynchronous readiness.

Covered behavior:

- Exact 640-byte capture from fragmented callbacks; excess and idle PCM is
  discarded instead of becoming a later request's audio.
- Capture timeout and stop during a pending capture leave the caller's output
  unchanged; stop-tail callbacks cannot complete the cancelled request.
- A second capture cannot steal a completed mailbox while its original caller
  has yet to consume it.
- New owners cannot consume previous-owner capture, and stale leases cannot
  stop or reconfigure the new owner. Stop discards unsent previous-owner playback.
- Recording continues after stopping a capture-only session without invoking
  destructive playback stop. A duplex interruption rebuilds VEM/I2S before
  accepting the next capture or playback, including the same lease and a
  released/reacquired owner.
  A failed rebuild or missing first callback tears down the partial restart
  and keeps the fault latched across retries and owner changes.
- Playback copies input before returning; subsequent caller edits cannot
  change the queued audio. Priming/tail silence does not duplicate payload.
  Ordinary SDK FINISH events cannot prematurely report the stream as done.
- Continuous 20-ms network packets, and a 60-ms burst followed by 20 ms,
  stay contiguous across 40-ms SDK slots. A short final tail waits a bounded
  interval before zero padding; stopping that partial wait discards old PCM.
- A play inserted while the stopping control task mutes remains blocked;
  the control stop gate covers mute and worker cleanup.
  On an active duplex session, scheduling the worker inside the codec mute
  call cannot start vendor teardown: mute finishes before that request is
  published. This reproduces the previously unprotected codec-write/teardown
  window; it does not by itself identify a board panic's cause.
- An old stop preempted after validation cannot cancel a new session when its
  caller reuses the same lease object. Release blocks play and a second release
  until its final owner change is committed, including after worker cleanup.
- Fifty 40-ms chunks maintain their rate with varying 2-5 ms SDK call costs;
  call overhead does not accumulate into a slower feeder or a queue overflow.
- A session idle for more than half the 32-bit clock range remains done and
  can start new playback at the normal slot rate, without a stale-clock burst.
  Capture and playback also work across clock wrap.
- All owners and ten levels retain the old software-gain arithmetic and
  codec mapping, including DEV unity gain and signed clipping boundaries.
- A full local playback queue rejects an entire submission, not a prefix;
  later accepted PCM appears only once at the SDK boundary.
- A vendor playback failure mutes the codec, runs worker cleanup and rejects
  nonzero level restoration or subsequent PCM instead of retrying accepted
  PCM. AEC/configuration/start failures cannot report ready.
  Worker creation failure and absence of the asynchronous first callback also
  fail preparation; the latter bounds the wait after the SDK start returns.
  This does not bound time spent inside the vendor start implementation.
- Codec mutex creation failure cannot report ready. Temporary level-update
  contention preserves the pending change, bounds its wait and retries once
  the mutex becomes available without taking down an active session.
- A stalled worker causes a bounded stop timeout and remains faulted. Release
  does not wait for that timeout a second time; late worker cleanup does not
  allow a new owner to bypass the reboot requirement.
- Playback stop can spend about 5 seconds waiting for its residual SDK ring
  after RX/DMA has stopped. A 5.1-second fake vendor stop remains within the
  10-second control budget, including the additional 200-ms muted drain grace,
  and permits a new owner to record and play.
  A separate scenario delays worker completion for 5.1 seconds while the
  foreground continues polling: this rejects the old 3.5-second budget,
  which a synchronous nested vendor fake alone would fail to detect.
  A completely frozen worker still times out at 10 seconds without a second
  full wait during release. This budget does not prevent assertions or bound
  an individual closed SDK call that never returns.
- Before stopping RX/VEM, a 200-ms grace period keeps the vendor duplex path
  alive with the codec muted. A small optional DMA model consumes one 1280-byte
  slot per 40 ms and checks that this grace can clear five residual slots.
  New application playback/capture remains blocked, no further PCM is submitted
  to the SDK, callbacks during the grace cannot populate a later capture, and
  neither the critical section nor the codec mutex is held while waiting.
  A second scenario prevents timer consumption: the same bounded grace ends,
  record-first cleanup proceeds through the vendor's 5-second fallback, and
  capture becomes usable again. These cases check the optimization and its
  fallback, not guaranteed real-board queue drainage within 200 ms.
- A recording-stop failure leaves playback teardown and capture restart
  unattempted, keeps the codec muted and rejects later owners. The fake
  explicitly exercises this failure even though the audited SDK wrapper
  currently returns success unconditionally.

The fakes deliberately do not claim that vendor stop establishes complete
callback quiescence. They inject callbacks while stopping and while no capture
request is open. They do not invent a callback generation token that the
vendor API does not supply. Actual stream startup, PA mute/drain timing,
long-running task scheduling and acoustic cancellation still require a board.
The capture fake normally schedules PCM for a requested frame; it does not
prove uninterrupted upstream sampling. The fresh-frame mailbox intentionally
drops ISR frames between capture requests, so foreground stalls require live
frame-rate measurement. Likewise, the playback transcript records accepted
bytes rather than a continuous DMA consumption clock. The two drain scenarios
add a deliberately limited timer model only during the stop grace; they cannot
prove long-run vendor queue occupancy or an exact audible completion time.

On 2026-10-09, all 38 scenarios passed with Group disabled and enabled (76
executions). After tightening the two new drain cases to feed eight actual
SDK slots before modeling five residual slots, both cases passed again in
both configurations. The unchanged 5.1-second slow-stop and deferred-worker
cases include the 200-ms grace, and the 10-second frozen-worker failure bound
still passes.

## Remote AI goodbye tail

`run_ai_remote_tail_test.py` extracts the production request gate, result
normalizer and tail helper, with deterministic queue/audio/clock boundaries.
It also checks the manager's real run/normalize/tail/cleanup call sequence.
The 53 AEC-enabled and 5 AEC-disabled cases passed: remaining and late audio,
retained playback under backpressure, 1200-ms quiet confirmation, a
3000-ms no-submission-progress guard, callback producers, the empty-queue/publication race,
BACK/new request/call/error/interrupt priority and 32-bit clock wrap.
Healthy playback lasting longer than three seconds keeps draining; late
packets at 250/900 ms remain audible in the model. Every successful submission
restarts the stall budget. The disabled branch does not call the tail helper.

`run_ai_remote_tail_command_test.py` extracts the production terminal-command
reader and uses the real project JSON guard with upstream cJSON 1.7.16.
Its 40 cases passed, including malformed/null/non-string/deep JSON, wrong
command words, interrupt decoding, slot release and the eight-slot work bound
under continuous arrival. New business actions are not dispatched. The command
must now be complete JSON-RPC 2.0 and an actual notification before it can
interrupt playback; a valid JSON prefix followed by garbage is rejected.

`run_ai_command_protocol_test.py` uses the same real parser with the production
dispatcher. Its 41 cases per AEC setting cover startup response matching and shape, compatible
omitted audio formats, invalid notifications, wire-length/NUL boundaries,
and preserving the existing valid call/round/end/interrupt actions. Interrupt
clears old audio before the blocking stop; a new reply injected during stop
remains queued afterward.

`run_ai_aec_loop_test.py` also checks that only AEC capture-frame timeouts get
a bounded retry: two seconds from the first timeout, with no encoding or
sending of failed captures. Playback continues during retries; success,
cancel, network backpressure, level changes and a new request reset the gap
budget. Persistent failures, real errors and clock wrap are covered. With
AEC disabled, the original immediate capture-error behavior remains. Lite
AI has no separate microphone-mute state; its SET levels are limited to 1–10.

```powershell
python examples/NT26FxDx_OpenKit/tirtc_lite/tests/run_ai_remote_tail_test.py --cc gcc
python examples/NT26FxDx_OpenKit/tirtc_lite/tests/run_ai_remote_tail_command_test.py --cc gcc --cjson PATH/TO/cjson-1.7.16/cJSON.c
python examples/NT26FxDx_OpenKit/tirtc_lite/tests/run_ai_command_protocol_test.py --cc gcc --cjson PATH/TO/cjson-1.7.16/cJSON.c
python examples/NT26FxDx_OpenKit/tirtc_lite/tests/run_ai_aec_loop_test.py --cc gcc
```

The second runner verifies the upstream C source hash; it does not download
dependencies. These tests model the audio boundary and cannot prove the
private SDK ring's audible completion. The new tail candidate still requires
board listening tests. The preceding candidate completed four logged exits
with result zero and a return to MENU, but the user heard its goodbye cut off.

## Burst buffering and first-play volume restoration

`run_ai_downlink_buffer_test.py` extracts production audio/command slot
management, callbacks, discard and playback functions. All 19 cases passed
with the actual AEC-dependent queue capacities (256 enabled, 16 disabled).
They include 40-packet bursts, 100 packets arriving at twice real time with
finite PCM backpressure, exact output ordering without duplication, all slots
including 255, explicit overflow accounting, format/decode/queue errors,
command-pool isolation and throttled retry logs. A callback injected after
discard's final empty read retains its live slot; the frozen old discard
function fails that ownership assertion in both configurations.

The audio adapter suite now has 45 scenarios, each passing with Group both
disabled and enabled (90 executions). New fakes reproduce the vendor's
asynchronous codec reset 1/40/120 ms after the third stream submission,
including speaker and microphone defaults. The actual production worker
detects drift using quantized readback and restores the settings even without
new foreground audio calls. Tests cover a failed read disguised as zero,
Stop/owner handoff, a worker snapshot resumed after Stop completed, and a
busy level mutex returning immediately without delaying the playback clock.
The prior production sources fail all six delayed-reset negative controls.
The AEC-disabled gain suite also passes unchanged.

```powershell
python examples/NT26FxDx_OpenKit/tirtc_lite/tests/run_ai_downlink_buffer_test.py --cc gcc
```

The volume fake models
register quantization and asynchronous resets, not I2C timing or a real
concurrent RTOS. A short default-volume window can remain before readback
correction. Buffer capacity is finite, and the network/codec boundaries are
simulated; extended speech, weak-network bursts, first-word loudness and
actual goodbye completion still require the new firmware on the board.
