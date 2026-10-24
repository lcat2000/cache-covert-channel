# cache-covert-channel

A minimal, self-contained proof of concept of a **cross-core CPU-cache covert
channel**. Two independent processes — with separate address spaces — exchange a
greeting (**Agent A says "早安", Agent B replies "您好"**) without using any pipe,
socket, signal, or shared variable. The only thing that carries the message is
the **cache state of a single cache line**, read out through access-timing
(Flush+Reload).

> ⚠️ **For security research and education only.** This is a *cooperating-process*
> demonstration of a well-documented micro-architectural phenomenon. It does not
> exploit a vulnerability, attack a victim, or exfiltrate anyone's data. Use it to
> understand, detect, and mitigate cache side/covert channels. See
> [Responsible use](#responsible-use).

---

## English

### What it demonstrates

```
Agent A  ──「早安」─▶   (E6 97 A9 E5 AE 89)  on cache line A→B
Agent B  heard      : E6 97 A9 E5 AE 89 → "早安"   ⇒ replies 您好
Agent A  heard reply: E6 82 A8 E5 A5 BD → "您好"

Handshake : SUCCESS ✅
Transport : Flush+Reload cache covert channel, cross-core
```

- Two processes are created with `fork()` → **separate address spaces**, pinned to
  **different physical cores** (core 0 / core 1).
- They share exactly **one `MAP_SHARED` page**. Inside it, two cache lines act as
  one-way channels (A→B and B→A).
- The greeting travels purely as **micro-architectural state**: whether a given
  cache line is currently resident or evicted.
- Each direction carries a framed message (preamble `AA AA` · SFD `7E` · length ·
  payload · XOR checksum), so Agent B only replies after it has *validated* that it
  received "早安".

### How it works

This is a Flush+Reload-style channel, with the modulation adapted to hardware
where two vCPUs do **not** share a fast last-level cache:

1. **Modulation (receiver-primes).** Each slot, the **receiver** primes the probe
   line into its *own* L1 by reading it. The **sender** then either:
   - leaves it alone → the line stays in the receiver's L1 → fast read → bit **1**, or
   - hammers `clflush` → a global eviction knocks the line out of the receiver's L1
     → slow read → bit **0**.

   `clflush` is architecturally system-wide, so one core's flush evicts another
   core's copy — that is the actual cross-core carrier here.

2. **Slot timing.** A shared TSC (`rdtscp`, `constant_tsc`) gives both processes a
   common time base divided into fixed slots. Each slot is **prime → (flush-or-idle)
   → sample**, with quiet margins at both ends so a slot's flushing never spills onto
   the neighbouring slot's prime.

3. **Reliability.** Each bit is sent `R` times and majority-voted to ride out
   isolated glitches. Because a free-running slot schedule desyncs if either process
   is preempted mid-frame, a driver process **re-forks a fresh A/B pair per attempt**
   and retries until the XOR-checked frame round-trips cleanly — standard
   covert-channel practice (retransmission).

### Measured results

On an Intel Xeon ~2.1 GHz, 2-vCPU VM (`clflush`/`clflushopt`/`rdtscp`,
`constant_tsc`/`nonstop_tsc`):

| Metric | Value |
|---|---|
| Access latency, bit 1 (survives in receiver L1) | ~84 cycles |
| Access latency, bit 0 (clflushed out) | ~306 cycles |
| Decision threshold | 187 cycles |
| Slot | 30,000 cycles (~14 µs), 3× repetition per bit |
| Raw rate | ~23 Kbit/s (one direction) |
| Reliability | 30/30 handshakes succeeded (driver retries through VM scheduling noise) |

Per-attempt success is lower on a busy shared VM (frequent preemption over the
~6 ms frame); on dedicated hardware single-frame success is far higher.

### Requirements

- x86-64 CPU with `clflush` and `rdtscp` (check: `grep -o -m1 clflush /proc/cpuinfo`).
- Linux. At least 2 cores (the demo pins to cores 0 and 1).
- GCC or Clang.

### Build & run

```sh
make          # or: gcc -O2 -Wall -o greet greet.c
./greet
```

### Defensive takeaways

- **Detection.** This pattern leaves clear PMU traces: abnormally high `clflush`
  rates, periodic L1/LLC miss bursts, and alternating cross-core access to the same
  line. Use the PoC as a labelled target when testing detection (perf counters,
  Intel CMT/MBM).
- **Mitigation.** Use it to measure how much channel capacity drops under cache
  partitioning (Intel CAT/RDT), core isolation, or scheduling that avoids sharing a
  physical core between untrusted tenants.

### Responsible use

This repository is intended for defenders, researchers, and students studying
micro-architectural side/covert channels. It demonstrates communication between two
*willing* processes and contains no exploit against any third party. Do not use it
to bypass isolation boundaries you are not authorised to test.

### References & attribution

The source code in this repository is original and does not incorporate any
third-party code, so no additional licenses apply. The underlying *technique* is
from published academic research and is cited here as attribution:

- Y. Yarom and K. Falkner. *FLUSH+RELOAD: A High Resolution, Low Noise, L3 Cache
  Side-Channel Attack.* USENIX Security Symposium, 2014.
- C. Maurice et al. *Hello from the Other Side: SSH over Robust Cache Covert
  Channels in the Cloud.* NDSS, 2017.

### License

[MIT](LICENSE) © 2026 Kai Chang (Morris)

---

## 中文（繁體）

### 這個專案在示範什麼

```
Agent A  ──「早安」─▶   (E6 97 A9 E5 AE 89)  在 cache line A→B 上
Agent B  收到        : E6 97 A9 E5 AE 89 → 「早安」  ⇒ 回覆「您好」
Agent A  收到回覆    : E6 82 A8 E5 A5 BD → 「您好」

交握結果 : SUCCESS ✅
傳輸方式 : Flush+Reload 快取隱蔽通道、跨核心
```

- 兩個程序以 `fork()` 產生,**各自獨立位址空間**,分別 pin 在**不同實體核心**(core 0 / core 1)。
- 它們只共用**一個 `MAP_SHARED` 實體頁**;頁裡兩條 cache line 當作兩個單向通道(A→B、B→A)。
- 訊息**完全以微架構狀態傳遞**——也就是某條 cache line 當下在不在快取裡。
- 每個方向都送出帶 framing 的訊息(前導碼 `AA AA`・SFD `7E`・長度・payload・XOR 校驗),所以 Agent B 是在**驗證**確實收到「早安」後才回「您好」。

### 運作原理

這是 Flush+Reload 式的通道,並針對「兩個 vCPU 不共用快速末級快取」的硬體調整了調變方式:

1. **調變(接收端先 prime)。** 每個時槽,**接收端**先讀取探測線,把它載入自己的 L1;接著**發送端**:
   - 什麼都不做 → 線留在接收端 L1 → 讀取快 → bit **1**;或
   - 狂敲 `clflush` → 全域逐出,把接收端 L1 裡的線也打掉 → 讀取慢 → bit **0**。

   `clflush` 在架構上是全系統生效,一個核心的 flush 會逐出另一核心的副本——這才是真正的跨核心載體。

2. **時槽同步。** 共用的 TSC(`rdtscp`、`constant_tsc`)讓兩個程序有一致的時間基準,切成固定時槽。每格的順序是 **prime → (flush 或 idle) → 取樣**,兩端各留靜默緩衝,避免某格的 flush 洩漏到下一格的 prime。

3. **可靠度。** 每個 bit 送 `R` 次並取多數決,過濾偶發雜訊。由於自由跑的時槽一旦任一程序在幀中被搶佔就會失步,driver 程序會**每次 attempt 重新 fork 一對全新 A/B 重新同步**,靠 XOR 校驗判斷,成功才停——這也是真實隱蔽通道的標準做法(重傳)。

### 實測結果

在 Intel Xeon 約 2.1 GHz、2 vCPU 的 VM 上(具備 `clflush`/`clflushopt`/`rdtscp`、`constant_tsc`/`nonstop_tsc`):

| 指標 | 數值 |
|---|---|
| bit 1 存取延遲(留在接收端 L1) | ~84 cycle |
| bit 0 存取延遲(被 clflush 逐出) | ~306 cycle |
| 判定閾值 | 187 cycle |
| 時槽 | 30,000 cycle(~14 µs),每 bit 重複 3 次 |
| 原始速率 | ~23 Kbit/s(單向) |
| 可靠度 | 30/30 次交握成功(driver 會穿過 VM 排程雜訊重試) |

在繁忙的共享 VM 上,單次 attempt 成功率較低(~6 ms 的幀期間容易被搶佔);在專用硬體上單幀成功率會高得多。

### 環境需求

- 支援 `clflush` 與 `rdtscp` 的 x86-64 CPU(檢查:`grep -o -m1 clflush /proc/cpuinfo`)。
- Linux,至少 2 核心(demo 會 pin 到 core 0 與 core 1)。
- GCC 或 Clang。

### 編譯與執行

```sh
make          # 或: gcc -O2 -Wall -o greet greet.c
./greet
```

### 防禦面重點

- **偵測。** 這種行為會在 PMU 上留下明顯痕跡:異常高的 `clflush` 頻率、週期性的 L1/LLC miss 爆發、對同一條線的跨核心交替存取。可把本 PoC 當作已標記的目標,測試偵測規則(perf counters、Intel CMT/MBM)。
- **緩解。** 可用它量測在 cache partitioning(Intel CAT/RDT)、核心隔離、或避免讓互不信任租戶共用實體核心的排程下,通道容量下降多少。

### 使用規範

本專案提供給研究微架構側通道/隱蔽通道的防禦者、研究人員與學生。它示範的是兩個**彼此合作**的程序之間的通訊,不含對任何第三方的攻擊。請勿用於未經授權的隔離邊界測試。

### 參考文獻與出處

本 repo 的原始碼為原創,未納入任何第三方程式碼,因此沒有額外授權需遵循。底層所用的「技術」出自公開學術研究,在此作為 attribution 引用:

- Y. Yarom and K. Falkner. *FLUSH+RELOAD: A High Resolution, Low Noise, L3 Cache Side-Channel Attack.* USENIX Security Symposium, 2014.
- C. Maurice et al. *Hello from the Other Side: SSH over Robust Cache Covert Channels in the Cloud.* NDSS, 2017.

### 授權

[MIT](LICENSE) © 2026 Kai Chang (Morris)
