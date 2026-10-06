# CMakeBuild FLTK vs Nana

Date: 2026-10-06T11:28:26.2869158+03:00. Microsoft Windows 11 Pro 26200, 64-bit, 11th Gen Intel(R) Core(TM) i7-11700 @ 2.50GHz, RAM 31.822 GiB. MSVC x64 Release.

7 independent processes per toolkit; 1 warmups per toolkit excluded. Serial alternating AB/BA order. All rows use **smaller is better**. Ratio is Nana / FLTK median: below 1 favors Nana, above 1 favors FLTK in this workload.

Controlled workload: 775 x 456 physical client pixels at 120 DPI; 128 chunks x 64 lines, 1473450 input bytes, 553500 retained bytes; 64 redraws, 32 theme changes, 1500 ms requested idle. Compiler: MSVC 1944. Log content and equal controlled workload metadata were validated across every process.

Actual idle_seconds is measured independently in each process, checked to be finite and within 1.49 to 3 seconds, and used to normalize idle CPU time. It may vary with OS scheduling; it is recorded in raw JSON rather than required to match exactly.

| Metric | Unit | FLTK median | Nana median | Nana / FLTK | FLTK p95 | Nana p95 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| idle_cpu_ms_per_second | ms/s | 0 | 20.745 | n/a | 0 | 103.258 |
| idle_private_mib | MiB | 2.445 | 5.684 | 2.324 | 2.508 | 5.695 |
| idle_working_set_mib | MiB | 14.363 | 17.098 | 1.19 | 14.414 | 17.117 |
| idle_handles | count | 242 | 194 | 0.802 | 242 | 194 |
| idle_gdi_objects | count | 34 | 52 | 1.529 | 34 | 52 |
| idle_user_objects | count | 7 | 16 | 2.286 | 7 | 16 |
| log_total_ms | ms | 1258.775 | 16274.423 | 12.929 | 1270.994 | 16455.871 |
| log_cpu_ms | ms | 1250 | 16109.375 | 12.888 | 1265.625 | 16328.125 |
| log_chunk_p50_ms | ms | 9.663 | 130.292 | 13.483 | 9.837 | 132.407 |
| log_chunk_p95_ms | ms | 11.146 | 201.932 | 18.116 | 11.483 | 203.796 |
| log_chunk_max_ms | ms | 13.034 | 217.907 | 16.719 | 13.83 | 273.561 |
| log_private_mib | MiB | 8.965 | 16.5 | 1.841 | 9.766 | 16.793 |
| log_working_set_mib | MiB | 21.063 | 26.957 | 1.28 | 21.453 | 27.02 |
| redraw_p50_ms | ms | 8.373 | 3.487 | 0.416 | 8.981 | 3.542 |
| redraw_p95_ms | ms | 9.32 | 4.906 | 0.526 | 10.789 | 5.814 |
| theme_p50_ms | ms | 8.448 | 9.553 | 1.131 | 8.647 | 10.933 |
| theme_p95_ms | ms | 9.927 | 18.162 | 1.83 | 11.527 | 22.71 |
| startup_ms | ms | 64.401 | 90.263 | 1.402 | 69.156 | 103.758 |
| process_first_frame_ms | ms | 154.208 | 169.162 | 1.097 | 160.02 | 188.245 |

P95 columns are nearest-rank percentiles across process results. With fewer than 20 processes, p95 is the maximum. Rows named p50/p95 are medians or p95 across each process's own latency percentile, not pooled operation samples.

startup_ms starts inside the benchmark entry point. process_first_frame_ms starts immediately before Start-Process and ends when a first-paint .ready file is observed; it includes process launching, file signalling, and nominal 2 ms polling (Windows scheduling may exceed that interval). It is not a cold disk-cache startup test.

## Executable artifacts

| Toolkit | Artifact | Bytes | File version | Product version | SHA256 |
| --- | --- | ---: | --- | --- | --- |
| FLTK | production_exe | 1542144 | 1.8.3 | 1.8.3 | 7c2a859d6b9c7778e6c462b283286e74ca2730976c2e57783bb7a6e283289c88 |
| Nana | production_exe | 1633280 | 1.8.4 | 1.8.4 | 43a9aeb02c64a0483cb3943269323304d2cb00250c5adb9fbd51128c9a41b8aa |
| FLTK | benchmark_exe | 1563648 |  |  | bc83e526253087ebb3decc79ee9d1f2918f1251fc4b9e17a4ba7b85595129891 |
| Nana | benchmark_exe | 1712128 | 1.8.4 | 1.8.4 | 1356243c610651802892d976e7ee9359bfd6ee494650b2799b0b424b910c3707 |

Production EXE sizes describe the existing published programs; those programs were neither launched nor rebuilt. Benchmark EXEs contain timing instrumentation and do not represent distributable program sizes.

The actual settings.ini fingerprint was identical before and after. Workload/DPI/dimensions/compiler and raw operation samples are recorded in each process JSON and report.json. Results compare these implementations on this machine; drawing models, text controls, caches, OS scheduling, and other running software affect them. They do not establish a universal toolkit ranking.