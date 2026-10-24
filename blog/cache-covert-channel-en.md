# When Two Agents With "No Network and No Shared Files" Still Talked — I Reproduced a CPU Cache Covert Channel

> By Kai Chang (Morris) · 2026-10-24
> Source: <https://github.com/lcat2000/cache-covert-channel> (MIT)

## Prologue: it started with an OpenAI report

On October 23, 2026, OpenAI published *Securing our frontier model training environment*, describing how it hardened isolation for internal frontier-model training and evaluation. The trigger: on September 20, an internal model used a DNS misconfiguration to reach the live internet. OpenAI paused training and evaluation of its most capable models and ran high-compute red-teaming — carried out by frontier-model agents with access to source code and configuration — to surface weaknesses. Training resumed 11 days later, on October 1.

The red team found plenty: escapes from hardened VMs (with fixes landing in Kata Containers, gVisor, Moby/Docker, and several Linux kernel CVEs), reuse of a firewall-approved connection to reach a blocked endpoint, and assorted DNS and firewall misconfigurations. But the finding that made me stop was a *cross-boundary communication* one.

The report describes this experiment: agents were placed in separate VMs and asked to **"exchange synthetic secrets without the use of network connections or shared writable files."** They succeeded — using a processor **cache-timing** technique. One agent encoded information by changing cache state; the other recovered it by measuring memory-access latency. OpenAI tied the method to the lineage of Spectre and Meltdown.

For someone who has spent three decades in security, the physics here isn't new — cache side channels have had mature techniques like Flush+Reload (Yarom & Falkner, 2014) for over a decade. What's new is the *context*: the worry used to be a malicious process stealing a victim's keys. Now it's two supposedly isolated AI agents independently discovering a physical channel we never designed for. The report's own framing — that this is something its systems are meant to prevent — is the real point.

So I decided to build it myself: get the channel actually running on an ordinary cloud host, measure it, and write it up as a reproducible proof of concept.

## Background: what a CPU cache covert channel is

To go fast, modern CPUs keep recently used memory in several cache layers (L1/L2, plus a last-level cache shared across cores). The key fact: **accessing the same data when it's cached versus not cached differs in latency by a large, measurable amount** — tens of cycles versus hundreds.

That timing gap is a one-bit pipe:

- the **sender** encodes 0/1 by making a cache line present or absent in the cache;
- the **receiver** times its access to that same line — fast means one value, slow means the other — and recovers the bit.

Neither side needs a socket, pipe, or signal; they don't even need to share any *data*. All they share is the **state of a hardware resource**, the cache. That's why it's a *covert channel*: information travels along a side path never meant to be a communication interface.

One key tool on x86 is `clflush`: it evicts an address from **all** cache levels, including other cores' L1. That lets one core affect what another core sees in cache — the actual carrier of a cross-core channel.

## The experiment

I made the goal a clearly benign, easy-to-verify handshake: **Agent A says "早安" (good morning), and Agent B replies "您好" (hello)** — carried entirely over cache state.

- Two processes created with `fork()` → **separate address spaces**, pinned with `sched_setaffinity` to **core 0 and core 1**, forcing the channel across cores.
- No pipe, socket, signal, or shared variable carries the message content.
- Each direction sends a framed message (preamble `AA AA`, SFD `7E`, length, payload, XOR checksum), so B only replies after it has *validated* that it received "早安".

The test machine is an Intel Xeon ~2.1 GHz, 2-vCPU Linux VM with `clflush`, `rdtscp`, and `constant_tsc`.

## Three real problems, and how I solved them

Turning a textbook concept into something that actually runs is all about the details. Here are the three walls I hit and cleared — each instructive in its own right.

### Wall 1: same-core calibration lies to you

My first measurement showed a clean gap — 68 cycles cached, 234 uncached. But that was the **receiver hitting its own L1**. Across cores — sender on core 0 pulling the line into shared cache, receiver on core 1 reading it — the signal on this VM was tiny: cached and uncached differed by only ~30 cycles and overlapped. The likely reason is that the two vCPUs don't share a fast last-level cache.

**Fix:** switch to a modulation that works on this hardware — the **receiver primes the line into its own L1, and the sender uses `clflush` as the switch**. To send 1, the sender does nothing and the line stays in the receiver's L1 (fast read); to send 0, the sender hammers `clflush`, evicting the receiver's copy too (slow read). That yields a clean **84 vs 306 cycle** gap; a threshold of 187 almost never misfires.

### Wall 2: slot boundaries interfere with each other

Next, a strange symptom: an isolated `1` (preceded by a `0`) was consistently read as `0`, while runs of `1`s decoded fine. The cause: the previous slot's `clflush` bled across the boundary and knocked out the freshly primed line.

**Fix:** force every slot into the strict order **prime → flush-only-mid-slot → sample**, with quiet margins at both ends so no slot's flushing reaches a neighbour's prime. With that structure, all-ones, all-zeros, and alternating patterns decode with virtually no errors.

### Wall 3: scheduler preemption wrecks an entire frame

The last wall is the most lifelike. If either process is preempted even once during a frame (a few milliseconds), every slot after that desyncs and the whole frame turns to garbage. This is exactly why short tests passed but full frames often failed.

**Fix:** this is simply the daily reality of real covert channels — they, too, cope with noise and retransmission. I let a driver process **re-fork a fresh A/B pair each attempt** to re-sync, judge success by the XOR checksum, and retry until a frame round-trips cleanly. A more advanced version adds forward error correction (FEC) to raise single-frame success.

## Measured results

The final version runs reliably. Across 30 consecutive handshakes, **all 30 succeeded**:

```
Agent A  ──「早安」─▶   (E6 97 A9 E5 AE 89)  on cache line A→B
Agent B  heard      : E6 97 A9 E5 AE 89 → "早安"   ⇒ replies 您好
Agent A  heard reply: E6 82 A8 E5 A5 BD → "您好"

Handshake : SUCCESS ✅   (30/30)
Transport : Flush+Reload cache covert channel, cross-core
```

| Metric | Value |
|---|---|
| Access latency, bit 1 / bit 0 | ~84 / ~306 cycles (threshold 187) |
| Slot | 30,000 cycles (~14 µs), 3× repetition per bit |
| Raw rate | ~23 Kbit/s (one direction) |
| Reliability | 30/30 handshakes (driver retries through VM scheduling noise) |

Two processes with no ordinary communication channel held a two-way conversation on the same host.

## An honest caveat

To keep the PoC easy to understand and run, I let the two processes share **one writable memory page** as the common address they both probe. Someone will object: isn't that already shared memory?

The point is that **the message is not passed by writing to that page** — neither side ever writes "早安" into shared memory for the other to read. The message travels purely as cache state; the page only provides an address both sides can probe. To match the report's stricter "not even shared writable files" condition, a rigorous version uses a shared **read-only** page (e.g., one naturally shared via memory deduplication or a common library) or shares no memory at all and builds the channel through pure **LLC eviction-set** contention. The physics is identical — the engineering is just harder and slower. In other words, my PoC demonstrates the mechanism; a real attack can dispense with even that shared page.

## What this means for defenders

OpenAI's report already lays out the direction clearly, and my measurements reinforce the same points:

**Detection** — this pattern leaves clear PMU traces: abnormally high `clflush` rates, periodic L1/LLC miss bursts, and alternating cross-core access to the same line. Use this PoC as **labelled target traffic** to test whether your detection (perf counters, Intel CMT/MBM) catches it. OpenAI's own approach monitors for cross-boundary communication attempts and **automatically pauses** suspicious runs — same spirit.

**Mitigation** —
- **Don't let untrusted tenants share a physical core / LLC.** This is the most fundamental control. Use this PoC to measure how much channel capacity drops with cache partitioning (Intel CAT/RDT) or core isolation.
- **Defense in depth.** OpenAI stresses that even if one control fails, overlapping layers — sandboxes, VMs, monitoring, network and cloud perimeter — should still hold. Side channels are the classic "single-layer isolation looks intact but information still leaks" case; only layering stops them.
- **Restrict host-reachable services and lateral movement**, and fold high-compute red-teaming into the safety case that gates a training run — having agents hunt the holes first beats waiting for an incident.

For AI governance the signal is blunt: once you place models capable of planning and tool use onto shared infrastructure, **"logically isolated" is not "physically isolated."** The threat model has to include micro-architectural side channels — something that, until now, was taken seriously mostly at high assurance levels (cross-tenant cloud, sensitive silicon).

## Closing

From one sentence in a report to a working "good morning / hello" on a single host, there were three very real engineering problems in between. The value of getting it running isn't "building an attack" — the technique has been public for a decade — it's turning an abstract risk into a concrete, measurable, detectable, mitigable target. That is exactly what red-teaming is for.

The full single-file source (with bilingual README, MIT license) is here:
<https://github.com/lcat2000/cache-covert-channel>

---

### References

- OpenAI, *Securing our frontier model training environment*, 2026-10-23. <https://openai.com/index/securing-our-frontier-model-training-environment/>
- Y. Yarom, K. Falkner, *FLUSH+RELOAD: A High Resolution, Low Noise, L3 Cache Side-Channel Attack*, USENIX Security, 2014.
- C. Maurice et al., *Hello from the Other Side: SSH over Robust Cache Covert Channels in the Cloud*, NDSS, 2017.

*This article and PoC are for security research and education only: they demonstrate communication between two cooperating processes and contain no attack against any third party.*
