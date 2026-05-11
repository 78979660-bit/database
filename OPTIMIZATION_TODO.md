# Optimization TODO

1. Batch the hot scan path.
   - Status: done.
   - Use direct columnar batch reads in `SeqScanExecutor::Next(Chunk&)` for all-integer scans without transaction visibility checks.

2. Reduce per-batch allocation.
   - Status: done.
   - Reuse JIT result buffers, null buffers, selection vectors, and vectorized benchmark result vectors.

3. Increase batch size to amortize loop and JIT call overhead.
   - Status: done.
   - `STANDARD_VECTOR_SIZE` is now 4096 instead of 1024.

4. Pre-reserve benchmark storage.
   - Status: done.
   - Reserve row and column storage before loading the synthetic benchmark table.

5. Isolate generated-code hot loops for future VTune work.
   - Status: done.
   - `test_benchmark_jit.exe --simd-only` now uses VTune ITT pause/resume so Microarchitecture Exploration attributes samples to SIMD/JIT query loops instead of data loading and JIT compilation.
   - `--repeat N` repeats each SIMD query inside the ITT resume window to produce a longer and more stable hardware-sampling interval.

6. Improve core utilization.
   - Status: benchmarked and optimized.
   - The benchmark now includes `Parallel SIMD (JIT)` backed by a persistent worker pool instead of per-query `std::async` thread creation.
   - `--parallel-only`, `--workers N`, and `--pin-pcores` isolate the parallel scan and keep worker threads on the likely P-core logical CPU range.
   - Worker scan contexts now reuse per-thread column pointers, result buffers, null buffers, and unpack buffers across scenarios.
   - Result counting now uses AVX2 non-zero counting instead of a scalar branch per row.

7. Reduce front-end pressure in generated code.
   - Status: in progress.
   - Batch JIT now uses magic multiply/shift for positive constant integer divisors, matching the scalar JIT fast path.
   - Batch-count JIT now directly returns the match count, avoiding result-vector stores and a second count pass in the benchmark hot path.
