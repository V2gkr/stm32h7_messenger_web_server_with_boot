# Hardware/firmware lessons learned

A running log of the non-obvious bugs this project has hit and how they were
found and fixed - mostly memory-domain, cache, DMA-concurrency, and
CubeMX-code-generation issues on the STM32H750. Written up once the
SD+Ethernet+USB combination stopped hard-faulting, as a reference before
starting the next round of feature work.

Each entry: **symptom** -> **root cause** -> **fix**, plus where the fix
lives so it isn't accidentally reverted later (CubeMX regenerates a lot of
this codebase's boilerplate, and it does not know about any of these fixes).

---

## 1. SDMMC IDMA buffer contending with Ethernet DMA on RAM_D2

**Symptom:** SDMMC RX_OVERRUN errors under load.

**Root cause:** The SDMMC1 IDMA transfer buffer lived in `RAM_D2` (AHB SRAM),
the same domain the Ethernet DMA descriptors/buffers use because of same MPU config .
In IDMA mode there is no connection between sdmmc1 and RAM_D2

**Fix:** Moved the SDMMC bounce buffer to `RAM_D1` (AXI SRAM,
`0x24000000`), a separate bus domain from Ethernet's `RAM_D2` traffic.
`RAM_D1` is cacheable, so this buffer also needed:
- 32-byte alignment (D-cache line size on Cortex-M7), and
- manual `SCB_CleanDCache_by_Addr()` / `SCB_InvalidateDCache_by_Addr()`
  around the DMA transfers - or, as later refined (see `mmc_transfer.c`),
  placing the buffer in the *non-cacheable* MPU sub-region carved out of
  `RAM_D1` (MPU region 3, 8KB at `0x24000000`) so no manual cache
  maintenance is needed for that specific buffer at all.

**Where it lives:** `firmware/Core/Src/mmc_transfer.c` (`s_bounce`,
`.mmc_bounce_sec`), `firmware/STM32H750XX_FLASH.ld`, MPU region 3 in
`MPU_Config()` (`main.c`).

---

## 2. USB MSC + FatFs both driving the SD card directly

**Symptom:** Data corruption / unreliable transfers when the SD card was
reachable both from the host over USB MSC and from the firmware's own FatFs
use at the same time.

**Root cause (architecture):** Both paths were issuing HAL_MMC calls
directly against the same peripheral with no serialization between them -
two independent callers of a single hardware resource.

**Root cause (the actual data corruption, once the architecture above was
in place):** in `MMC_WriteBlocks()`, `SCB_CleanDCache_by_Addr(s_bounce,
...)` was being issued **before** the `memcpy(s_bounce, src, ...)` that
filled the buffer, so the just-copied data stayed dirty in D-cache and
SDMMC1's IDMA read stale RAM contents off to the card - every USB MSC
write put garbage on disk. Two things obscured it: comments in that file
claimed `s_bounce` was already non-cacheable (it wasn't - plain `.bss`,
no section attribute), and the symptom looked like a *read*/enumeration
problem (host saw the disk but found no valid partition table, and the
garbage bytes on a re-scan of the same LBA were inconsistent between
scans - the tell that it was live corruption, not a formatting question).

**Fix:** Rewrote the whole SD access path around one dedicated consumer:
`MemoryTask` owns the SDMMC peripheral exclusively and is the only thing
that ever touches `HAL_MMC_*Blocks_DMA`. Both the USB MSC path
(`usbd_storage_if.c`) and the FatFs diskio path (`user_diskio.c`) now just
post a request (`FsDataStruct`) onto `memoryqueueHandle` and block on a
task notification for the result; `MemoryTask` drains that queue and calls
`MMC_ProcessRequest()` for every request, one at a time. Getting this
right took three architecture bug passes (an ISR-can't-block-on-task
deadlock, a stale-event condition across USB bus resets, and a
buffer-pointer staleness bug) plus the cache-ordering fix above before it
held up under sustained testing. `s_bounce` was later also moved into a
dedicated non-cacheable MPU sub-region (see #1) so the manual
Clean/Invalidate calls could be removed entirely rather than just
reordered.

**Where it lives:** `firmware/Core/Src/mmc_transfer.c`,
`firmware/FATFS/Target/user_diskio.c`, `firmware/Core/Src/usb_msc_task.c`,
`MemoryTask` in `main.c`.

**Status: resolved** (confirmed on hardware - USB MSC enumerates and the
host mounts the card correctly after a reformat). This is the
architecture currently in place. If anything resembling the old
intermittent corruption resurfaces, re-check this queue path and the
cache-maintenance ordering first before looking elsewhere.

---

## 3. `fsdata.c`: one shared `FIL` for every HTTP connection

**Symptom:** Serving more than one file via the custom `fs_open_custom` /
`fs_read_custom` HTTPD hooks corrupted whichever transfer was already in
flight.

**Root cause:** The original implementation had exactly one `static FIL
fsfile` (plus one `static UINT byte_read_ptr`) shared by *every* HTTP
connection. lwIP's httpd can have multiple `struct fs_file` contexts open
at once (interleaved connections, all processed on `tcpip_thread`); a
second `fs_open_custom()` call would `f_open()` over the same `FIL`
struct an earlier, still-streaming connection was using, corrupting its
read position mid-transfer.

**Fix:** Each open file now gets its own heap-allocated
`struct fs_custom_state { FIL fil; }`, allocated with `mem_malloc()` in
`fs_open_custom()` and stashed in `struct fs_file::pextension` - the field
lwIP's `fs.h` provides for exactly this purpose. `fs_close_custom()` frees
it. Chose the **lwIP heap** (`mem_malloc`/`mem_free`, `MEM_SIZE` in
`lwipopts.h`) over FreeRTOS's `heap_4` or the libc/newlib heap: all three
are small on this MCU, but the FatFs/httpd calls only ever run on
`tcpip_thread` anyway (see #6), so cross-task allocator locking wasn't the
deciding factor - keeping this lwIP-triggered allocation inside lwIP's own
heap was the more contained choice. `MEM_SIZE` was bumped from the lwIP
default (1600B) to 8192B to give it room (a `FIL` is roughly 560B with
this project's FatFs config).

**Where it lives:** `firmware/Core/Src/fsdata.c`.

---

## 4. `fsdata.c`: wrong end-of-file signal caused multi-second stalls

**Symptom:** Pages loaded, but with a large, inconsistent delay before the
browser considered the response finished - worse than it should have been
even accounting for #3 above.

**Root cause:** `fs_read_custom()` returned `0` at end-of-file. lwIP's
httpd (`fs.c` / `httpd.c`) only treats a **negative** return
(`FS_READ_EOF`, -1) as "done" - `0` is treated as "read zero bytes,
keep going." Since the custom-file path also never advanced
`file->index` (lwIP's own `fs_read()` wrapper skips that bookkeeping for
custom files, see `fs.c`), `fs_bytes_left()` never reported zero either,
so nothing short-circuited the retry. The only thing that eventually
noticed the connection was done was `http_poll()`, gated by
`HTTPD_POLL_INTERVAL` (~2s per tick) and `HTTPD_MAX_RETRIES` (4) - up to
~8 seconds of dead time per request in the worst case, because no new TCP
segment was ever generated to trigger a faster retry.

**Fix:** `fs_read_custom()` now returns `FS_READ_EOF` (not `0`) once
`f_read()` reports zero bytes or an error, and `file->index` is tracked
(zeroed in `fs_open_custom()`, incremented per read). The latter also lets
`httpd.c`'s own fast-path put the FIN flag on the last data segment
directly instead of waiting for the next poll cycle.

**Where it lives:** `firmware/Core/Src/fsdata.c`.

---

## 5. lwIP heap (`MEM_SIZE`) silently overlapping the Ethernet RX pool

This is the big one - the root cause of the intermittent HardFaults hit
while testing #3/#4 above, and it predates all of this session's changes
(reproduced on `main` before any `fsdata.c` work).

**Symptom:** Random `HardFault`s (`PRECISERR` bus faults) while browsing
the web server, most often but not exclusively after a second/third
request in quick succession (e.g. `favicon.ico` right after `index.html`).
Not reliably reproducible - frequency scaled with how much concurrent
SD-card + Ethernet activity was happening (e.g. much more reliable with a
USB MSC session also active).

**Root cause:** `lwipopts.h`'s `LWIP_RAM_HEAP_POINTER` - CubeMX's
hard-coded default heap anchor for H7 parts - pins the lwIP heap
(`mem_malloc`/`mem_free`) to the fixed address `0x30004000` in `RAM_D2`.
The Ethernet zero-copy RX buffer pool (`.lwip_sec`, `Rx_PoolSection`, 12
buffers x ~1536B+overhead) is placed starting at `0x30000100` in the same
domain - and, confirmed via the linker `.map` file, actually **ends at
`0x30004a83`**, about 2.6KB *past* the heap's fixed start address. Every
`mem_malloc()`/`mem_free()` call (httpd's per-response TX buffer, and now
`fsdata.c`'s per-connection `FIL`) was writing into memory still being
used as one of the last 2-3 Ethernet RX DMA buffers, and vice versa - two
independent, unsynchronized writers (the Ethernet DMA engine and the CPU's
heap allocator) sharing the same physical bytes. Intermittent because it
only bit when an in-flight RX buffer happened to be one of the
overlapping slots *and* a heap operation touched that same region at the
wrong moment; more reliable under heavier load because more heap churn
means more chances to hit the window. `addr2line` on the fault address
pointed at `ethernet_input()` dereferencing a pbuf's `payload` -
consistent with a corrupted/already-overwritten RX buffer.

This was never caught by the linker because `LWIP_RAM_HEAP_POINTER` is a
raw address baked into a `#define`, invisible to the linker's
section-overlap detection - `.lwip_sec` is a real, linker-placed section,
but nothing tracked how far it actually extends relative to that magic
number.

**Fix:** Replaced the fixed address with a linker-provided symbol placed
immediately after `.lwip_sec`, so the heap can never again start inside
still-live RX buffer memory - and added an `ASSERT()` so that if
`.lwip_sec` or `MEM_SIZE` ever grow enough to collide with the next fixed
section (`.mmc_dma_sec`, USB MSC's buffer, anchored at `0x30008000`), the
**build fails** instead of corrupting RAM silently.

**Where it lives:**
- `firmware/STM32H750XX_FLASH.ld` - new `.lwip_heap_sec` section (reserves
  12KB right after `.lwip_sec`) plus the `ASSERT`.
- `firmware/LWIP/Target/lwipopts.h` - `USER CODE BEGIN 1` block:
  `extern char _lwip_heap_start; #undef LWIP_RAM_HEAP_POINTER; #define
  LWIP_RAM_HEAP_POINTER (&_lwip_heap_start)`.

**CubeMX regeneration caveat:** `LWIP_RAM_HEAP_POINTER 0x30004000` is not
a `.ioc`-configurable parameter - it's CubeMX's unconditional default for
H7 parts and will be re-emitted verbatim on every "Generate Code." The
override in `USER CODE BEGIN 1` survives regeneration and keeps winning
(the `#undef`+redefine happens after CubeMX's own `#define`), so no action
is needed after a regen - but don't delete that block, and don't "clean
up" it away thinking it's redundant with the line above it.

---

## 6. `httpd_init()` / `MX_LWIP_Init()` called from an unrelated task

**Symptom:** Not independently confirmed as a crash cause this session
(the RAM overlap in #5 was sufficient to explain everything observed), but
a real correctness issue found during cleanup and fixed alongside it.

**Root cause:** `MX_LWIP_Init()` (which calls `netif_add()`,
`netif_set_up()`, etc.) and `httpd_init()` (which calls `tcp_new()` /
`tcp_bind()` / `tcp_listen()`) are all lwIP "raw API" - per lwIP's
threading rules, these must only run on `tcpip_thread` (or be marshalled
onto it via `tcpip_callback()`) once the stack is up, because they mutate
global PCB/netif lists that `tcpip_thread` also owns without any locking
(this project doesn't use `LWIP_TCPIP_CORE_LOCKING`). Both calls used to
happen from `StartMemoryTask` - the SD-card queue-processing task -
*after* `osKernelStart()`, i.e. concurrently with `tcpip_thread` and the
Ethernet RX task (`EthIf`) already running. It happened to not lose the
race in practice because `StartMemoryTask` runs at `osPriorityHigh`,
above `tcpip_thread`'s priority (24) - but that's incidental, not a
guarantee, and doesn't hold once anything in the init path blocks.

**Fix:** Moved the call to `MX_LWIP_Init()` (with `httpd_init()` now
folded into the end of `MX_LWIP_Init()` itself, instead of being a
separate call bolted onto an unrelated task) into `main()`, **before**
`osKernelStart()`. Before the scheduler starts, nothing else is running -
`tcpip_thread` and `EthIf` are created moments later in that same call
chain but don't get to run until the scheduler starts - so there is no
race to reason about at all, provably.

**Where it lives:** `firmware/Core/Src/main.c` (`USER CODE BEGIN 2`,
before `osKernelInitialize()`), `firmware/LWIP/App/lwip.c` (`httpd_init()`
now called from `USER CODE BEGIN 3`, end of `MX_LWIP_Init()`).

**CubeMX regeneration caveat:** unlike #5, this one *is* tied to a
`.ioc`-level setting - which task CubeMX assigns to initialize the LwIP
middleware (currently `MemoryTask`). The `MX_LWIP_Init();` call removed
from `StartMemoryTask` was auto-generated boilerplate, not `USER CODE` -
regenerating with the same `.ioc` setting will put it back in
`StartMemoryTask`, causing `MX_LWIP_Init()` to run twice (once there,
once from `main()`). To make this permanent, reassign LwIP's init task in
CubeMX (Project Manager / Middleware RTOS settings) instead of relying on
this being hand-fixed after every regen.

---

## General takeaways for next time

- **RAM_D1 vs RAM_D2 vs RAM_D3 matters on H7**, not just as a capacity
  question - it determines which DMA masters you're sharing bus bandwidth
  with. Before parking a new DMA-touched buffer somewhere, check what else
  already lives in that domain (`STM32H750XX_FLASH.ld` has the map).
- **Cacheable vs non-cacheable is a second, independent axis** from the
  above - a buffer can be in the "right" RAM domain and still need manual
  cache maintenance, or can be carved into a non-cacheable MPU sub-region
  to avoid needing it. Check `MPU_Config()` in `main.c` before assuming
  either way.
- **A magic fixed address is a landmine if nothing enforces it.**
  `LWIP_RAM_HEAP_POINTER` (#5) is the cautionary example: a
  linker-placed, size-tracked section with an `ASSERT` against its
  neighbors is strictly better than a hand-picked hex constant, even if
  the constant "looks like" it should have room.
- **CubeMX-generated code and hand-written fixes don't mix safely by
  default.** Anything outside a `USER CODE` block gets silently
  overwritten on the next "Generate Code." Before editing generated
  boilerplate directly, check whether the same effect can be achieved by
  reassigning the relevant setting in CubeMX's GUI instead - and if not,
  document the regeneration caveat right next to the fix (as done above)
  so it isn't a surprise months later.
- **"Only idle task runs" / total freeze points at a mutex or queue wait
  that never gets satisfied**, not necessarily at the thing that's
  visibly failing - trace back to what every blocked task is waiting on,
  not just the one that's easiest to see.
- **Intermittent + "gets worse under unrelated extra load" is close to a
  guaranteed sign of a shared-memory/timing race**, not a logic bug - a
  logic bug reproduces the same way every time given the same input.
