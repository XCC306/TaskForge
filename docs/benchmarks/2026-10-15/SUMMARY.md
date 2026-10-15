# TaskForge verification measurements

These are end-to-end CLI measurements on this machine, not universal speedup claims.

| Case | Mode | Workers | Median ms | Min ms | Max ms | MiB/s | Speedup |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| tiny | serial | 1 | 13.925 | 12.670 | 15.416 | 7.109 | 1.000x |
| tiny | parallel | 1 | 13.129 | 12.105 | 16.452 | 7.540 | 1.061x |
| tiny | parallel | 2 | 12.024 | 11.028 | 13.504 | 8.233 | 1.158x |
| tiny | parallel | 4 | 11.317 | 10.344 | 13.727 | 8.748 | 1.230x |
| medium | serial | 1 | 41.164 | 39.532 | 45.585 | 355.873 | 1.000x |
| medium | parallel | 1 | 41.721 | 40.144 | 48.650 | 351.120 | 0.987x |
| medium | parallel | 2 | 26.895 | 26.561 | 33.799 | 544.678 | 1.531x |
| medium | parallel | 4 | 21.067 | 20.576 | 30.812 | 695.355 | 1.954x |
| large_stereo | serial | 1 | 229.154 | 222.977 | 230.564 | 383.546 | 1.000x |
| large_stereo | parallel | 1 | 228.430 | 226.152 | 281.279 | 384.760 | 1.003x |
| large_stereo | parallel | 2 | 133.989 | 126.498 | 136.234 | 655.956 | 1.710x |
| large_stereo | parallel | 4 | 83.661 | 78.545 | 114.743 | 1050.566 | 2.739x |

Warmups are excluded. OS caches were not flushed. Each measured result was checked.
Speedup = serial median / selected-mode median; values below 1 indicate a slowdown.
Scope includes startup, I/O, analysis, stdout capture and thread shutdown.
See results.json, runs.jsonl, fixture hashes and per-run logs for evidence.
