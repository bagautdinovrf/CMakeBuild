# CMakeBuild FLTK vs Nana

Date: 2026-10-06T12:25:12.2538478+03:00. Microsoft Windows 11 Pro 26200, 64-bit, 11th Gen Intel(R) Core(TM) i7-11700 @ 2.50GHz, RAM 31.822 GiB. MSVC x64 Release.

7 independent processes per toolkit; 1 warmups per toolkit excluded. Serial alternating AB/BA order. All rows use **smaller is better**. Ratio is Nana / FLTK median: below 1 favors Nana, above 1 favors FLTK in this workload.

Controlled workload: 775 x 456 physical client pixels at 120 DPI; 128 chunks x 64 lines, 1473450 input bytes, 553500 retained bytes; 64 redraws, 32 theme changes, 1500 ms requested idle. Compiler: MSVC 1944. Log content and equal controlled workload metadata were validated across every process.

Actual idle_seconds is measured independently in each process, checked to be finite and within 1.49 to 3 seconds, and used to normalize idle CPU time. It may vary with OS scheduling; it is recorded in raw JSON rather than required to match exactly.

| Metric | Unit | FLTK median | Nana median | Nana / FLTK | FLTK p95 | Nana p95 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| idle_cpu_ms_per_second | ms/s | 0 | 0 | n/a | 0 | 0 |
| idle_private_mib | MiB | 2.441 | 5.695 | 2.333 | 2.531 | 5.875 |
| idle_working_set_mib | MiB | 14.344 | 17.184 | 1.198 | 14.395 | 19.324 |
| idle_handles | count | 231 | 184 | 0.797 | 231 | 209 |
| idle_gdi_objects | count | 31 | 52 | 1.677 | 31 | 52 |
| idle_user_objects | count | 7 | 16 | 2.286 | 7 | 18 |
| log_total_ms | ms | 1245.357 | 1093.457 | 0.878 | 1363.906 | 1180.54 |
| log_cpu_ms | ms | 1250 | 1078.125 | 0.863 | 1359.375 | 1156.25 |
| log_chunk_p50_ms | ms | 9.569 | 7.909 | 0.827 | 10.406 | 8.183 |
| log_chunk_p95_ms | ms | 10.695 | 12.014 | 1.123 | 13.091 | 16.816 |
| log_chunk_max_ms | ms | 13.644 | 15.763 | 1.155 | 17.238 | 30.035 |
| log_private_mib | MiB | 8.965 | 18.688 | 2.085 | 10.238 | 18.879 |
| log_working_set_mib | MiB | 21.043 | 29.391 | 1.397 | 21.965 | 31.586 |
| redraw_p50_ms | ms | 8.433 | 3.625 | 0.43 | 9.174 | 3.942 |
| redraw_p95_ms | ms | 9.574 | 4.805 | 0.502 | 13.54 | 6.469 |
| theme_p50_ms | ms | 9.044 | 9.445 | 1.044 | 10.229 | 10.996 |
| theme_p95_ms | ms | 10.066 | 20.782 | 2.065 | 15.623 | 59.545 |
| startup_ms | ms | 70.405 | 95.864 | 1.362 | 71.878 | 97.559 |
| process_first_frame_ms | ms | 155.27 | 190.927 | 1.23 | 179.001 | 209.904 |

P95 columns are nearest-rank percentiles across process results. With fewer than 20 processes, p95 is the maximum. Rows named p50/p95 are medians or p95 across each process's own latency percentile, not pooled operation samples.

startup_ms starts inside the benchmark entry point. process_first_frame_ms starts immediately before Start-Process and ends when a first-paint .ready file is observed; it includes process launching, file signalling, and nominal 2 ms polling (Windows scheduling may exceed that interval). It is not a cold disk-cache startup test.

## Executable artifacts

| Toolkit | Artifact | Bytes | File version | Product version | SHA256 |
| --- | --- | ---: | --- | --- | --- |
| FLTK | production_exe | 1542144 | 1.8.3 | 1.8.3 | 7c2a859d6b9c7778e6c462b283286e74ca2730976c2e57783bb7a6e283289c88 |
| Nana | production_exe | 1636864 | 1.8.5 | 1.8.5 | 00f87e92db1789d1372dc14cfc6422cf729dceb156f6acbf8ebb05c87a1d2e76 |
| FLTK | benchmark_exe | 1563648 |  |  | bc83e526253087ebb3decc79ee9d1f2918f1251fc4b9e17a4ba7b85595129891 |
| Nana | benchmark_exe | 1716736 | 1.8.5 | 1.8.5 | c71418714464051bf10b4844062f5e696a9c53cf5c5ee9222247d655d7fdc34b |

Production EXE sizes describe the existing published programs; those programs were neither launched nor rebuilt. Benchmark EXEs contain timing instrumentation and do not represent distributable program sizes.

The actual settings.ini fingerprint was identical before and after. Workload/DPI/dimensions/compiler and raw operation samples are recorded in each process JSON and report.json. Results compare these implementations on this machine; drawing models, text controls, caches, OS scheduling, and other running software affect them. They do not establish a universal toolkit ranking.