<!-- SPDX-License-Identifier: Apache-2.0 -->

# Processor and component watchdogs

The watchdog extends processor stop policy. A silent processor is
not contained: it may hold a lock or an incomplete transaction, so detection
stops the system with `Lockup`, never offlines it. An AP can preserve evidence
when CPU 0 stops input, init and display together. Eligible AMD64 components
can run on APs; their user ticks must keep these heartbeats live.

## Configuration

BOOT.CFG accepts one optional `watchdog = SECONDS` (decimal 1–60) or
`watchdog = off`, after `version`. The default is five seconds. Invalid values
and duplicate keys refuse the manifest before loading files. Zero in the
kernel configuration means off. BootInfo v19 and later carry the UInt64
configuration at offset 106,480. The pager start block carries the same seconds,
so init uses exactly the boot-selected period. Power facts follow this word.
Existing finite task-exit supervision remains separate from service liveness.
Supervision waits wake at the checking cadence without converting those short
wakes into task-exit expiry; input-free product init also keeps servicing faults
and liveness. Three additional root-owned reply Endpoints are charged to the
existing management grant.

The final
reserved word of each ProtectionDomain creation form carries an optional diagnostic
component kind (zero or a ComponentBootstrap behavior). It confers no
authority, is immutable after publication, and supplies the stop label without
reading user memory or taking a domain lock. The 64-byte boot-only form uses
offset 56; the 80-byte form retains its Manage-authorized FaultChannel handle
at 56 and uses offset 72. Billing handles remain zero. The fault-supervisor
test behavior is a valid diagnostic kind too. ProtectionDomain creation enforces
FaultChannel ownership, live-tag uniqueness and the 64-byte form's caller restriction.
The desktop session exposes a root-Manage-only Session Query method, with the ordinary
Reply and a private reply Endpoint; clients acquire no new rights.

## Processor detection

A silent target is a Lockup only after the observer itself has made progress
for the configured period. Hypervisors may suspend every vCPU while the
ITC/TSC follows host time; elapsed counter time alone cannot distinguish that
pause from a processor failure. This applies to both ISAs, including early
boot before users run.

Each admitted processor owns an atomic heartbeat sequence and diagnostic
metadata. Timer ticks and scheduling points advance it, recording the kernel
site and running thread/component. Each observer retains its own last check
counter and heartbeat, and each peer's sequence and qualified silence age.
A changed peer sequence resets that age. An advanced observer heartbeat
credits the smaller of elapsed local counter time and one checking cadence;
a coalesced tick never credits missing ticks. No observer heartbeat advance
means no new credit. A first sample, counter regression, readmission or a
whole period without an observer check resets peer observations. Thus the
first observer to resume after a whole-machine stall cannot accuse the other
processors before they get a chance to publish their resumed heartbeats.

The detector returns every online peer with a full period of qualified age.
Diagnostic age and P4 use that same age, saturated at the configured period.
No subtraction of a peer's counter, allocation or lock is required; every
scan retains the four-processor bound. Admission clears the observer's clock
and peer samples. Offline/parked peers are excluded by the processor set's
mask; an excluded observer cannot accuse peers. The terminal power fence
suppresses checks before CPUs acknowledge parking, so a parked CPU is never
required to resume or answer a watchdog request during shutdown.

Processor admission initializes counters. During that
processor's bounded clock rendezvous it continues publishing heartbeats but
does not age its own peer observations. After a hardware adjustment it clears
those observations and starts a fresh interval; a forward counter jump cannot
manufacture a lockup. Other processors continue checking its heartbeat.

The one-shot timer must retain a watchdog wake independently of scheduler
and TimerQueue deadlines. AMD64 caps its hardware countdown at the watchdog
wake without changing the controller's authoritative scheduler receipt;
an earlier watchdog interrupt acknowledges/rearms and returns without
pretending that a run stop fired. IA-64's normal PL3 timer entry advances the
heartbeat. Its PSR.i-clear idle/service loops also service counter-derived
watchdog ticks in normal context. These polled ticks deliberately preserve
the existing PL0 interrupt-entry corridor and AP timer-proof contracts;
a loop stalled without polls is silent. A 250 ms checking cadence and the
five-second default comfortably exceed TCG's 100 ms processor turns. A late
tick is coalesced into one check; it never accumulates fictional missed ticks.

`Lockup` parameters are P1=stalled processor mask, P2=period in milliseconds,
P3=detecting processor, P4=maximum qualified heartbeat age in milliseconds.
The global stop latch is acquired before any freeze NMI is sent, so an NMI
cannot become an unrelated UnexpectedNmi stop. AMD64 sends the silent targets
first, then freezes the remaining online peers. Each NMI target records its
interrupted RIP, RFLAGS, CR3 and the active thread/component on its private
IST stack, even with IF clear. No NMI handler takes a lock or waits for an
interrupted metadata writer. Atomic metadata words permit a bounded snapshot;
an interrupted publication is explicitly shown as updating.

IA-64 has no OS_INIT contract and no mask-independent interrupt in this design.
A stalled PSR.i-clear CPU cannot expose its current IIP, IPSR, registers or
thread stack to another CPU. Its record remains unconfirmed and the screen
shows its last published heartbeat site and thread/component. Cooperative
peers retain the existing stop snapshot (IIP/IPSR and interruption registers);
those interruption registers may describe an earlier interruption rather than
the instant the poll stopped. The detector never invents a stalled CPU's IIP.
The watchdog uses no additional translation register. Stop evidence stores common
watchdog samples after ApplicationLaunch evidence, outside the
assembly-owned stop slots.

The stop screen shows each CPU's heartbeat age, location and component. A
confirmed AMD64 location is its captured RIP; an unconfirmed location is the
last heartbeat site, explicitly labelled. Kernel locations are displayed as
KERNEL.ELF section plus offset when within linked section bounds; user
locations retain their raw address and component name. Symbol names and full
register decoding remain available to the runner from KERNEL.ELF.

At least one processor must continue checking. A hang where every CPU stops
taking ticks and making scheduling progress is outside this design, as are
firmware/SMM hangs that block NMIs. A PMU NMI watchdog is not implemented.

## Component liveness

Init periodically sends an asynchronous, bounded Query to the display broker,
Input Broker and an installed session compositor. Every monitored service has
a private root-owned reply Endpoint and a transaction, so input events,
startup reports and drawing fences remain owned by their existing consumers.
Start/Launch registers services unready with the startup allowance below;
registration does not start live Queries or miss aging. The display broker
reserves a reply slot for the root's
high-transaction Manage Query and answers it before rendering; ordinary
Queries keep their drawing fence. Both retain immutable WouldBlock replies,
and unread ordinary fences cannot consume that reserved slot.
A Query reply carries no attachment. Immediately after Receive, root closes every imported
attachment using the canonical descriptor/attachment offset, including foreign or undecodable
frames, before checking method/transaction/status. Any attachment rejects the liveness reply.
Only a matching successful reply clears a pending check. Queue pressure,
missing answers and failed sends are misses; a rejected send is retried once
per poll within the original deadline, so transient pressure cannot orphan a
check. Supervision never waits
indefinitely for a liveness reply. Readiness gates live Queries while initial
scanout clearing remains covered by the separate startup deadline.

The bounded service table has five slots,
adding root-started auth and login to display/input/compositor. Auth reaches
Ready only after full 16,384-page arena admission, store validation and
limiter recovery. Login reaches Ready only after its current trusted bitmap
has passed the display broker's draw fence. Both use the same independent
60-second startup gate. Auth answers bounded Queries at public Argon2 segment
boundaries; root never hashes. Failure locks the seat and invalidates the
transaction; a stopped owner is drained rather than restoring a cached
success. Live login/auth restart and passwordless fallback are unsupported.

An Input Broker or compositor launch
registers a startup phase before live Queries begin. The first successful
Input Broker manager reply ends its phase; the first compositor FrameShown
and the subsequent Show fence prove its first Presented and visible native
scanout, ending the compositor/broker startup phase.
An already-ready display broker temporarily defers its pending live Query
while that first native session scanout completes. The owner cannot extend
the launch deadline by reinstalling its recovery hook. The allowance is
60 seconds on the monotonic clock, independent of the selected live watchdog
period or `off`. Expiry reports once as
`# startup: <component> not ready within 60000 ms`; no live miss or restart is
manufactured before readiness. Startup message waits return to their owner
at that deadline; the product tears down the incomplete session, restores
console focus and emits `# session: fallback startup-timeout`. Failed GUI
startup resumes the previously ready console broker's live supervision.
Test compositor orchestration marks readiness from its existing Presented
report.

After one unanswered period init writes exactly
`# watchdog: <component> unresponsive for <n> ms` to the diagnostic channel and
retains bounded evidence (component, transaction, duration, misses and recovery
outcome). A service silent for three periods is terminated through the
ComponentSupervisor, drained and restarted through the owning orchestration's
recovery hook where one exists. Recovery must republish endpoint/epoch links;
it cannot blindly revive a device driver or leave consumers on dead handles.
The console display hook republishes the root console and a fresh epoch.
While a session face has consumers it only reports, because the present
session orchestration has no reconnect hook. Input Broker
reports without restart because its owner has no persistent client-rebinding
path. The product compositor has a root-owned recovery hook: it is stopped,
detached and drained, then its native pools/input client are rebound for two
replacement incarnations. The third compositor death returns to the console.
Test compositors without an owner hook still only
report; the supervisor never invents a reconnect path.
The existing device-manager quiet proof and three-restart bound continue to
apply to USB/xHCI recovery. Services without an installed recovery hook remain
running and are reported; init does not claim a restart that it cannot wire.
Off disables both processor detection and these liveness Queries.

The liveness reply Endpoints are charged to the root's management grant,
never to a monitored component's restricted account. Its three extra Endpoint
units and queue/metadata reservations fit the inherited per-type capacity of
48; existing child Code authority and explicit account
limits remain intact. Queries neither read nor bind a FaultChannel. Faults and
exits continue through the domain's channel and pager protocol 1.2, carrying
the kernel ProtectionDomain identity. Recovery still terminates the thread,
serves pending channel deliveries, reaps it and drains the account before a
fresh incarnation is registered. A separate supervisor's channels and replies
remain owned by that supervisor; root liveness cannot consume its fault token.

## Governing specifications and validation

AMD64 delivery and capture use Intel, *Intel 64 and IA-32 Architectures Software
Developer's Manual*, revision 093 (325462-093US), Volume 3A §§6.3.1, 6.7.1,
6.14.5 (NMI and IST), and §§13.6.1, 13.12.9 (xAPIC/x2APIC ICR).
IA-64 counter and masking follow Intel, *IA-64 Architecture Software Developer's
Manual*, Volume 2, revision 1.1 (245318-002), §§3.3.4.2 and 5.8 (interval timer
and external interrupt control).

Host state-machine tests cover bounded legitimate kernel work, threshold
expiry, simultaneous silent peers, admission/retirement, clock regression and
off. Test-firmware-only stop probes stall with interrupts disabled after
startup; VM checks inspect the stalled mask, screen pixels and retained records,
including AMD64 RIP within the probe. A test service withholds Query replies;
its owning supervisor's restart and watchdog diagnostic are checked. Pause/resume
tests require desktop input to recover without a kernel stop or service restart.
Counter-discontinuity tests distinguish whole-machine gaps from stalled peers
after actual observer progress.
