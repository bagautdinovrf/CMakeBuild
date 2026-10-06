# CMakeBuild FLTK vs Nana

Date: 2026-10-06T12:16:55.6114560+03:00. Microsoft Windows 11 Pro 26200, 64-bit, 11th Gen Intel(R) Core(TM) i7-11700 @ 2.50GHz, RAM 31.822 GiB. MSVC x64 Release.

7 independent processes per toolkit; 1 warmups per toolkit excluded. Serial alternating AB/BA order. All rows use **smaller is better**. Ratio is Nana / FLTK median: below 1 favors Nana, above 1 favors FLTK in this workload.

Controlled workload: 775 x 456 physical client pixels at 120 DPI; 128 chunks x 64 lines, 1473450 input bytes, 553500 retained bytes; 64 redraws, 32 theme changes, 1500 ms requested idle. Compiler: MSVC 1944. Log content and equal controlled workload metadata were validated across every process.

Actual idle_seconds is measured independently in each process, checked to be finite and within 1.49 to 3 seconds, and used to normalize idle CPU time. It may vary with OS scheduling; it is recorded in raw JSON rather than required to match exactly.

| Metric | Unit | FLTK median | Nana median | Nana / FLTK | FLTK p95 | Nana p95 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| idle_cpu_ms_per_second | ms/s | 0 | 0 | n/a | 0 | 10.317 |
| idle_private_mib | MiB | 2.441 | 5.871 | 2.405 | 2.457 | 5.887 |
| idle_working_set_mib | MiB | 14.363 | 19.328 | 1.346 | 14.371 | 19.348 |
| idle_handles | count | 233 | 210 | 0.901 | 233 | 210 |
| idle_gdi_objects | count | 31 | 52 | 1.677 | 31 | 52 |
| idle_user_objects | count | 7 | 18 | 2.571 | 7 | 18 |
| log_total_ms | ms | 1220.257 | 1355.633 | 1.111 | 1347.507 | 1385.169 |
| log_cpu_ms | ms | 1187.5 | 1343.75 | 1.132 | 1328.125 | 1359.375 |
| log_chunk_p50_ms | ms | 9.435 | 7.516 | 0.797 | 9.92 | 7.663 |
| log_chunk_p95_ms | ms | 10.461 | 8.403 | 0.803 | 14.507 | 8.641 |
| log_chunk_max_ms | ms | 11.87 | 201.043 | 16.937 | 16.856 | 212.454 |
| log_private_mib | MiB | 8.957 | 16.691 | 1.863 | 9.953 | 17.066 |
| log_working_set_mib | MiB | 21.047 | 29.203 | 1.388 | 21.926 | 29.285 |
| redraw_p50_ms | ms | 8.213 | 3.464 | 0.422 | 8.841 | 3.505 |
| redraw_p95_ms | ms | 9.052 | 4.201 | 0.464 | 11.187 | 4.467 |
| theme_p50_ms | ms | 8.423 | 9.081 | 1.078 | 8.497 | 9.451 |
| theme_p95_ms | ms | 9.314 | 17.768 | 1.908 | 9.762 | 19.042 |
| startup_ms | ms | 66.33 | 98.635 | 1.487 | 68.278 | 101.553 |
| process_first_frame_ms | ms | 145.047 | 182.062 | 1.255 | 155.362 | 199.89 |

P95 columns are nearest-rank percentiles across process results. With fewer than 20 processes, p95 is the maximum. Rows named p50/p95 are medians or p95 across each process's own latency percentile, not pooled operation samples.

startup_ms starts inside the benchmark entry point. process_first_frame_ms starts immediately before Start-Process and ends when a first-paint .ready file is observed; it includes process launching, file signalling, and nominal 2 ms polling (Windows scheduling may exceed that interval). It is not a cold disk-cache startup test.

## Executable artifacts

| Toolkit | Artifact | Bytes | File version | Product version | SHA256 |
| --- | --- | ---: | --- | --- | --- |
| FLTK | production_exe | 1542144 | 1.8.3 | 1.8.3 | 7c2a859d6b9c7778e6c462b283286e74ca2730976c2e57783bb7a6e283289c88 |
| Nana | production_exe | 1636352 | 1.8.5 | 1.8.5 | 8d6cb9f8484e0889624d8dd2917066e8a35e82ad3c4c1d3a51a9afdb399c6540 |
| FLTK | benchmark_exe | 1563648 |  |  | bc83e526253087ebb3decc79ee9d1f2918f1251fc4b9e17a4ba7b85595129891 |
| Nana | benchmark_exe | 1715712 | 1.8.5 | 1.8.5 | 8e8de5b7889efe7c993fcfb82f778ba35a152f85cec0525ec73848f699999b67 |

Production EXE sizes describe the existing published programs; those programs were neither launched nor rebuilt. Benchmark EXEs contain timing instrumentation and do not represent distributable program sizes.

The actual settings.ini fingerprint was identical before and after. Workload/DPI/dimensions/compiler and raw operation samples are recorded in each process JSON and report.json. Results compare these implementations on this machine; drawing models, text controls, caches, OS scheduling, and other running software affect them. They do not establish a universal toolkit ranking.