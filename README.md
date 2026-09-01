# sensor-pipeline

A real Linux character-device kernel driver bound to an I2C sensor client,
sampling it on a kernel thread into a `kfifo` ring buffer and exposing
readings through `/dev/sensor0`, paired with a userspace daemon that drains
the device using a custom fixed-size memory-pool allocator (no `malloc` in
the sampling hot path).

## What it does

- **`driver/sensor_i2c.c`** — a Linux kernel module that:
  - registers a character device at `/dev/sensor0`
  - registers a real `i2c_driver` (`bme280_stub`) that probes onto an I2C
    client and reads its chip-id and calibration registers via
    `i2c_smbus_read_byte_data()`
  - starts a `kthread` that re-samples a measurement register every 500ms
    and pushes each reading into a `kfifo` ring buffer
  - blocks a userspace `read()` on `/dev/sensor0` until a sample is queued,
    then hands back one formatted line (`seq=... ts_ns=... raw=0x..`) per
    `read()` call
- **`daemon/sensord.c`** — a userspace daemon that reads `/dev/sensor0` in a
  loop and decodes each line into a `Sample` struct drawn from
  **`daemon/pool_allocator.c`**, a fixed-size free-list memory pool: one
  `malloc()` call up front allocates the whole backing buffer, and every
  sample after that is handed out and reclaimed with pointer arithmetic
  only — no `malloc`/`free` in the per-sample path.
- **`tests/test_pool_allocator.c`** — a standalone unit test for the
  allocator (capacity, exhaustion, free-then-reuse, zero-on-alloc). Runs
  directly on the host, no kernel or VM required.

## Hardware re-scoping — read this first

**No physical Raspberry Pi, BME280 sensor, or logic analyzer was available
to build this.** The machine this was built and verified on is a Mac
(Apple Silicon), which cannot build or load a Linux kernel module for
itself, and Docker Desktop's own Linux VM runs a bespoke `linuxkit` kernel
with no matching headers package available.

Instead of skipping hardware verification or faking it, this project uses
the same tools a kernel driver developer reaches for when hardware isn't on
hand yet:

- **[`i2c-stub`](https://www.kernel.org/doc/html/latest/i2c/i2c-stub.html)**
  is a real, in-tree Linux kernel module built for exactly this purpose: it
  creates a virtual I2C adapter that answers real SMBus transactions from a
  plain byte-addressable register file. `scripts/seed_stub.sh` seeds that
  register file to mimic a BME280's chip-id, calibration and measurement
  registers, then instantiates `sensor_i2c`'s client on the stub adapter.
  The driver talks to it through the exact same `i2c_smbus_read_byte_data()`
  calls it would use against real silicon.
- The module is built and `insmod`'d inside a **[Lima](https://lima-vm.io/)
  Ubuntu Linux VM** running its own real kernel with matching
  `linux-headers-$(uname -r)` installed from Ubuntu's own archive — a
  genuine build/load target, not an emulation shortcut.
- **No logic analyzer or sigrok capture was used.** An earlier draft of this
  project's CV bullet claimed a bus-timing defect was diagnosed via a
  logic-analyzer capture. That did not happen, and this README does not
  pretend otherwise. What actually happened is below, under Engineering
  notes.

What would differ against real hardware: real I2C bus timing and clock
stretching, real NACKs and bus errors, real sensor noise instead of
constant/seeded values, and a real logic-analyzer trace instead of
`i2c-stub`'s in-memory register file.

## Run it

Needs a real Linux box (or VM) with `build-essential`, matching
`linux-headers-$(uname -r)`, `i2c-tools`, and `kmod`. This project was built
and verified with:

```
brew install lima
limactl start template://default
limactl shell default
sudo apt-get install -y build-essential i2c-tools kmod
```

Then, inside that Linux environment:

```
git clone https://github.com/williamhuang1261/sensor-pipeline.git
cd sensor-pipeline
sudo bash scripts/run_demo.sh 8
```

`scripts/run_demo.sh` builds the driver and daemon, seeds `i2c-stub`,
loads the driver, runs `sensord` for the given sample count, then unloads
everything cleanly. `docs/demo-transcript.txt` is the verbatim captured
output of a real run of that script from a fresh `git clone`:

```
=== dmesg (probe output) ===
[  675.501371] i2c-stub: Virtual chip at 0x76
[  675.508303] i2c i2c-0: new_device: Instantiated device bme280_stub at 0x76
[  675.509680] bme280_stub 0-0076: chip id 0x60 confirmed
[  675.509682] bme280_stub 0-0076: probed at 0x76, calibration bytes: 1a 2b 3c 4d
[  675.509736] bme280_stub 0-0076: poll thread started, interval 500 ms
[  675.509768] sensor_i2c: loaded, /dev/sensor0 ready (major 510), i2c driver 'bme280_stub' registered

=== Running sensord ===
sensord: reading 8 samples from /dev/sensor0
sample #0: seq=1 ts_ns=675283524387 raw=0x50
sample #1: seq=2 ts_ns=675795907689 raw=0x50
...
sample #7: seq=8 ts_ns=678825676406 raw=0x50

=== Tearing down ===
Clean unload complete.
```

Full transcript: [`docs/demo-transcript.txt`](docs/demo-transcript.txt).

Unit tests (no VM needed, runs on the host):

```
make -C tests
```

## Engineering notes

**A real debugging incident, not the originally planned one.** While
proving the kthread really re-samples the bus live (rather than caching one
read forever), the natural test was to change the stub's measurement
register with `i2cset` while the driver was loaded and watch the next
`read()` pick up the new value. That failed with `Error: Could not set
address to 0x76: Device or resource busy`. The cause: `i2c-dev` refuses a
userspace SMBus transaction to an address a kernel driver has already bound
a client to, specifically to prevent a userspace tool and a kernel driver
racing the same device. The fix was `i2cset -f` (force), which bypasses
that check for exactly this kind of test/debug scenario. With that,
flipping the register mid-stream showed the sample sequence's `raw` value
change from `0x50` to `0x99` starting at the very next scheduled sample
(`seq=45` in one captured run), proving the kthread reads the live register
value on every poll rather than a cached one.

**Why a `kfifo` and a kernel thread instead of reading the sensor directly
inside `read()`.** Sampling only when userspace calls `read()` would mean a
slow or idle reader silently causes the sensor to be polled less often, and
a burst of interest right after a slow patch would only see one stale
value. Decoupling sampling (the kthread, on its own fixed interval) from
consumption (`read()`, draining the fifo) means the sampling rate is
independent of how often anything is listening, at the cost of a fixed
32-sample buffer that drops the oldest entry if nothing drains it in time
(`kfifo_skip` when full) rather than blocking the kthread indefinitely.

**Why a free-list pool instead of just not allocating at all.** The sample
struct is fixed-size and known ahead of time, so the daemon could equally
have used one static/stack-allocated struct reused every iteration with no
allocator at all. The pool exists to demonstrate the general pattern (a
one-time backing allocation, then O(1) alloc/free by pointer arithmetic)
in a form that scales past "exactly one struct" without code changes if a
future consumer needed to hold several samples in flight at once — the
`POOL_CAPACITY` in `daemon/sensord.c` is already sized for that, unused
headroom today.

## What's deliberately not here

- No burst multi-byte reads (a real BME280 needs a burst read across several
  registers per measurement, plus the documented compensation formula
  applied to raw ADC counts); this project reads one measurement byte per
  poll to keep the ring-buffer plumbing the focus.
- No `ioctl` configuration interface (poll interval, fifo depth) — both are
  compile-time constants (`POLL_INTERVAL_MS`, `FIFO_CAPACITY`).
- No sysfs attributes for reading the last sample without going through
  `/dev/sensor0`.
- No real hardware verification, as stated above.
