# DPA Characterization

Benchmarks and experiments for the DPA on NVIDIA BlueField-3, using the DOCA SDK and FlexIO.

## Compute and Thread Benchmarks

- `cpu-arch` — Measures aggregate IPC under growing number of threads, with three kernels.
- `dpa-asm` — Compile and link hand-written RISC-V assembly to DPA APP, DPA SDK and FlexIO.
- `dpa-compute-type-bench` — Measures single thread integer and software floating-point arithmetic latency and throughput, with additional same-core and multicore concurrency benchmarks. Build with assembly.
- `dpa-inter-thread/cache-invalidation` — Verify the cache invalidation effect of spreading placement policy.
- `dpa-offloading-model` — Evaluates offloading overhead and efficiency for compute and pointer-chasing workloads, comparing host execution with DPA kernel launches and persistent threads.

## Memory Copy Benchmarks

Under `copy-bench`, evaluate DPA-Host data movement latency and bandwidth through different mechanisms and software abstraction.

- `dpa-async-memcpy-bench` — Asynchronous memory Copy
- `dpa-external-ptr-copy-bench` — External pointers via Memory Aperture
- `dpa-comch-copy-bench` — Comch DMA
- `dpa-flexio-mw-copy-bench` — FlexIO memory windows, via Memory Aperture
- `dpa-verbs-copy-bench` — DOCA Verbs

## Launch Latency Benchmarks

Under `launch-bench`, these projects measure launching and triggering latency for DOCA and FlexIO execution paths.

- `dpa-launch-latency-bench` — DOCA DPA RPC, kernel launch and Event Handler
- `dpa-flexio-launch-latency-bench` — FlexIO RPC and Event Handler
