#!/usr/bin/env python3
from pathlib import Path

root = Path("PebbleOS/third_party/nonfree/pebbleos-nonfree")

spo2 = root / "gh3x2x/algo_lib/algo_params/SPO2/goodix_spo2_config_for_gh3x2x-v2.23_7ecd2a.c"
text = spo2.read_text()
replacements = {
    ".acc_thr_max = 50,": ".acc_thr_max = 250,",
    ".acc_thr_min = 25,": ".acc_thr_min = 80,",
    ".ppg_jitter_thr = 35,": ".ppg_jitter_thr = 60,",
    ".ppg_noise_thr = 25,": ".ppg_noise_thr = 45,",
    ".ppg_coeff_thr = 85,": ".ppg_coeff_thr = 60,",
    ".fast_out_time = 12,": ".fast_out_time = 8,",
    ".min_stable_time_low = 7,": ".min_stable_time_low = 4,",
}
for old, new in replacements.items():
    if old not in text:
        raise SystemExit(f"Missing expected SpO2 config value: {old}")
    text = text.replace(old, new, 1)
spo2.write_text(text)

regs = root / "gh3x2x/demo_code/demo_kernel_code/kernel/gh_demo_reg_array.c"
text = regs.read_text()

# Only patch the active #if 1 register list. These are the exact values proposed in
# coredevices/pebbleos-nonfree PR #4 for red/IR convergence on the wrist.
start = text.index("#if 1")
end = text.index("#endif", start)
active = text[start:end]
reg_replacements = {
    "{0x011E, 0x0019}": "{0x011E, 0x0090}",
    "{0x0122, 0x0CBF}": "{0x0122, 0x0CFF}",
    "{0x013C, 0x0219}": "{0x013C, 0x0290}",
    "{0x013E, 0x197F}": "{0x013E, 0x19FF}",
    "{0x0156, 0x0219}": "{0x0156, 0x0290}",
    "{0x015A, 0x197F}": "{0x015A, 0x19FF}",
}
for old, new in reg_replacements.items():
    if old not in active:
        raise SystemExit(f"Missing expected active register value: {old}")
    active = active.replace(old, new, 1)
text = text[:start] + active + text[end:]
regs.write_text(text)

print("Applied upstream experimental SpO2 wrist tuning")
