# Benchmark VM

Reported runs happen on a dedicated Windows VM that is **reverted to a snapshot before every run**; Wine is only
for developing the harness.

1. Install Windows and Python 3.10+ in the guest, and nothing else that touches processes (no other debugger or
   RE tool unless the configuration under test brings it).
2. Build the targets on the host: `bench/build.sh <out>` (mingw).
3. In the guest, elevated: `vm\prepare.ps1 -Cortex <pinned Cortex build> -Targets <out>`.
4. Take the snapshot. Record its name and `C:\bench\vm_state.json` (hashes) in the preregistration addendum.
5. Per run: revert (see `revert_snapshot.sh.example`), then in the guest
   `python C:\bench\harness\harness\runner.py --task ... --config ... --seed N --cortex C:\bench\cortex\cortex.exe --targets C:\bench\targets --model <frozen id> --out C:\bench\results`
   and copy `C:\bench\results\<run>` out before the next revert.
6. The Cheat Engine configurations (A, B) need the pinned CE and bridge installed in the same snapshot; their server
   command goes in `configs/A.json` and `configs/B.json` once the baseline is pinned.

Before the first run that counts, run `tests\run_security_regression_vm.ps1` in the guest, without `--skip-tool`.
