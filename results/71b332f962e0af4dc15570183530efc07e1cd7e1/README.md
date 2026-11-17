# bench-x86 run 44027950500 at 71b332f962e0af4dc15570183530efc07e1cd7e1

Raw output of .github/workflows/bench-x86.yml (https://github.com/AtharvaPatil466/Order-Matching-Engine/actions/runs/44027950500), landed unchanged
by `scripts/import_bench_results.sh 44027950500`. docs/PERF_SOURCES.md names the
file each doc number comes from.

## Environment (env.txt, verbatim)

```
commit: 71b332f962e0af4dc15570183530efc07e1cd7e1
date_utc: 2026-11-17T06:00:32Z
worktree_changes: 1 (git status --porcelain lines)
runner: GitHub Actions 1000002411 image: ubuntu24 20261111.127.1
Linux ip-10-0-129-171 7.0.0-1014-aws #14~24.04.1-Ubuntu SMP PREEMPT Fri Sep 18 21:24:59 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux
Model name:                              Intel(R) Xeon(R) 6975P-C
--- nproc: 4
--- governor: unreadable
Ubuntu clang version 18.1.3 (1ubuntu1)
cmake version 3.31.6
--- liburing: /usr/include/liburing.h
journal_fs: ext2/ext3
4096000 bytes (4.1 MB, 3.9 MiB) copied, 0.214523 s, 19.1 MB/s
--- pinning: taskset -c 1
--- workload: REPS=5 ORDERS=50000 RF_EVENTS=500000 RF_SPARSE_EVENTS=300000 CO_ORDERS=200000 CO_RATES="100000 500000 1000000 2000000 3000000 4000000 6000000" CC_ORDERS=20000 SUSTAIN_SEC=20
```

## Files

```
build-native.txt
build-pgo.txt
build-prod.txt
co-100000.txt
co-1000000.txt
co-2000000.txt
co-3000000.txt
co-4000000.txt
co-500000.txt
co-6000000.txt
co-poisson-100000.txt
co-poisson-1000000.txt
co-poisson-2000000.txt
co-poisson-3000000.txt
co-poisson-4000000.txt
co-poisson-500000.txt
co-poisson-6000000.txt
codec.txt
cold-cache.txt
configure-native.txt
configure-prod.txt
env.txt
exit-codes.txt
honest-native-1.txt
honest-native-2.txt
honest-native-3.txt
honest-native-4.txt
honest-native-5.txt
honest-pgo-1.txt
honest-pgo-2.txt
honest-pgo-3.txt
honest-pgo-4.txt
honest-pgo-5.txt
honest-prod-1.txt
honest-prod-2.txt
honest-prod-3.txt
honest-prod-4.txt
honest-prod-5.txt
memory-probe.txt
perf-prod-a.txt
perf-prod-b.txt
perf-prod-c.txt
perf-prod-d.txt
perf-prod-l.txt
realistic-flow-sparse.txt
realistic-flow.txt
stress.txt
sustained-400k.txt
sustained-50k.txt
```
