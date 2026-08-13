## Vivado Synthesis Status (2026-03-28 18:04)

- **Attempt 1 (salty-zephyr, yesterday):** All 12 sub-modules done, main process crashed before top-level synth
- **Attempt 2 (resume_synth.tcl, 15:58):** vrs deadlocked for 2 hours due to stale .vivado.begin.rst + reset_run wiping all sub-module states
- **Attempt 3 (run_impl.tcl -force, 18:03):** Currently running - PID 725052, actively generating BD
  - IP cache should speed up sub-module synths
  - Expect conv_ip ~1h, total ~2h
  - Check at ~20:15 for completion

**Vivado PID:** 725052
**Output log:** E:\codes\microzed\fastvit_hls\vivado_impl\run_impl_out.log
