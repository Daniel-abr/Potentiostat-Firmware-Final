"""Exported from palmsens_vs_openstat.ipynb.

Usage: python palmsens_vs_openstat.py [data folder containing "Palmsens" and "My Device"]
"""
import os
import sys

os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')


# # PalmSens4 vs OpenStat — DPV comparison (Sunday 11 Oct)
#
# Compares the PalmSens reference scans in `Palmsens/*.pssession` against the OpenStat scans in
# `My Device/*.csv`.
#
# Both instruments ran the same DPV method: 0.6 V → −0.2 V, 5 mV step, 50 mV pulse, 50 ms pulse time,
# ~0.2 s interval (0.025 V/s).
#
# **What gets flipped to make them comparable**
#
# | Thing | PalmSens | OpenStat (as saved) | Fix applied here |
# |---|---|---|---|
# | Current sign | negative (≈ −0.2 … −0.5 µA) | positive (`dI_nA` ≈ +170 … +960 nA) | OpenStat `dI` × −1 |
# | Current unit | µA | nA | PalmSens × 1000 → nA |
# | Scan direction | 0.6 → −0.2 V | same, **except `sample_81`** which ran −0.2 → 0.6 V at a 0.5 s interval | plotted vs E so it lines up, but kept out of the statistics |
# | X axis | PSTrace draws 0.6 V on the left | — | x axis drawn 0.6 → −0.2 V to match PSTrace |

import json
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

HERE = Path.cwd()                      # run this notebook from the "Sunday Oct 11" folder
PS_DIR = HERE / 'Palmsens'
DEV_DIR = HERE / 'My Device'

DEVICE_SIGN = -1                       # OpenStat dI is sign-inverted relative to PalmSens
FOCUS = 'Salt_3_2'                    # run compared on its own in section 9 (kept out of the group mean)
TRANSIENT_CUTOFF_V = 0.45              # points above this are dominated by the start-of-scan transient

plt.rcParams.update({'figure.dpi': 110, 'axes.grid': True, 'grid.alpha': 0.3,
                     'axes.spines.top': False, 'axes.spines.right': False})
plt.show()

# ## 1. Load both data sets

def load_pssession(path):
    # .pssession = UTF-16 JSON with a BOM (and sometimes trailing junk)
    raw = Path(path).read_bytes().decode('utf-16').strip('﻿\x00 \r\n')
    m = json.loads(raw)['Measurements'][0]

    method = {}
    for line in m['Method'].splitlines():
        if '=' in line and not line.startswith('#'):
            k, v = line.split('=', 1)
            method[k] = v

    arrays = {a['Description']: np.array([p['V'] for p in a['DataValues']])
              for a in m['DataSet']['Values']}
    df = pd.DataFrame({'t_s': arrays['time'],
                       'E_V': arrays['potential'],
                       'dI_nA': arrays['current'] * 1e3})      # stored in uA
    return df, method


def load_device(path):
    df = pd.read_csv(path)
    return pd.DataFrame({'t_s': (df.t_ms - df.t_ms.iloc[0]) / 1e3,
                         'E_V': df.E_applied_V,
                         'dI_raw_nA': df.dI_nA,                 # exactly as saved
                         'dI_nA': DEVICE_SIGN * df.dI_nA,       # flipped to PalmSens convention
                         'I_before_nA': df.I_before_nA,
                         'I_end_nA': df.I_end_nA,
                         'rtia_code': df.rtia_code})


def drop_identical(runs):
    # some files are byte-for-byte copies of another run - keep only the first of each
    kept, dupes = {}, []
    for name, df in runs.items():
        twin = next((k for k, d in kept.items()
                     if len(d) == len(df) and np.array_equal(d.dI_nA.values, df.dI_nA.values)), None)
        if twin is None:
            kept[name] = df
        else:
            dupes.append(f'{name} == {twin}')
    return kept, dupes


ps_all, ps_method = {}, {}
by_name = lambda p: (len(p.stem), p.stem)        # 'sample_83' before 'sample_83 (1)'
for p in [PS_DIR / f'{n}.pssession' for n in ['Salt_1', 'Salt_2', 'Salt_3', 'Salt_4']]:
    ps_all[p.stem], ps_method[p.stem] = load_pssession(p)
dev_all = {p.stem: load_device(p) for p in [DEV_DIR / f'{n}.csv' for n in ['sample_81', 'sample_82', 'sample_83', 'sample_83 (1)', 'sample_84',
                                         'sample_85', 'sample_86', 'Salt_2_1', 'Salt_2_2', 'Salt_3_2']]}

ps, ps_dupes = drop_identical(ps_all)
dev, dev_dupes = drop_identical(dev_all)
print('Identical copies dropped:', ps_dupes + dev_dupes)

# runs that used the same scan direction as PalmSens (first E > last E)
dev_same = {k: d for k, d in dev.items() if d.E_V.iloc[0] > d.E_V.iloc[-1] and k != FOCUS}
dev_other = {k: d for k, d in dev.items() if k not in dev_same and k != FOCUS}
print('Same direction as PalmSens:', list(dev_same))
print('Different scan direction  :', list(dev_other))
print(
    print('Compared separately (sec. 9):', FOCUS if FOCUS in dev else 'not found')
)

rows = []
for name, d in {**ps, **dev}.items():
    rows.append({'run': name,
                 'instrument': 'PalmSens' if name in ps else 'OpenStat',
                 'points': len(d),
                 'E start [V]': round(d.E_V.iloc[0], 4),
                 'E end [V]': round(d.E_V.iloc[-1], 4),
                 'step [mV]': round(d.E_V.diff().mean() * 1e3, 3),
                 'interval [s]': round(d.t_s.diff().mean(), 4),
                 'dI min [nA]': round(d.dI_nA.min(), 1),
                 'dI max [nA]': round(d.dI_nA.max(), 1),
                 'RTIA code': '' if name in ps else ','.join(map(str, d.rtia_code.unique()))})
print(
    pd.DataFrame(rows).set_index('run')
)

m = ps_method[next(iter(ps))]
print(
    pd.Series({k: m[k] for k in ['E_BEGIN', 'E_END', 'E_STEP', 'E_PULSE', 'T_PULSE', 'SCAN_RATE',
                                 'T_EQUIL', 'IRANGE_MIN', 'IRANGE_MAX', 'IRANGE_START', 'SMOOTH_LEVEL']},
              name='PalmSens method')
)

# ## 2. As recorded — before any flipping
#
# Left: PalmSens. Right: OpenStat `dI_nA` exactly as saved. Same shape family, opposite sign.

def psaxis(ax):
    ax.set_xlim(0.62, -0.22)           # PSTrace orientation: start potential on the left
    ax.set_xlabel('E [V]')

ps_colors = plt.cm.Oranges(np.linspace(0.45, 0.9, len(ps)))
dev_colors = plt.cm.viridis(np.linspace(0.0, 0.85, len(dev_same)))

fig, ax = plt.subplots(1, 2, figsize=(13, 4.5))
for (k, d), c in zip(ps.items(), ps_colors):
    ax[0].plot(d.E_V, d.dI_nA, color=c, label=k)
for (k, d), c in zip(dev_same.items(), dev_colors):
    ax[1].plot(d.E_V, d.dI_raw_nA, color=c, label=k)
for k, d in dev_other.items():
    ax[1].plot(d.E_V, d.dI_raw_nA, color='crimson', ls='--', label=f'{k} (reverse scan)')
ax[0].set_title('PalmSens (as recorded)'); ax[1].set_title('OpenStat (as recorded)')
for a in ax:
    psaxis(a); a.set_ylabel('ΔI [nA]'); a.legend(fontsize=8)
plt.tight_layout()
plt.show()

# ## 3. Overlay — OpenStat sign flipped to match PalmSens
#
# The first plot is the full range; the second zooms the y axis so the start-of-scan spike on the OpenStat
# (left edge, near 0.6 V) doesn't squash everything else.

def overlay(ax, include_other=True):
    for (k, d), c in zip(ps.items(), ps_colors):
        ax.plot(d.E_V, d.dI_nA, color=c, lw=2, label=f'PalmSens {k}')
    for (k, d), c in zip(dev_same.items(), dev_colors):
        ax.plot(d.E_V, d.dI_nA, color=c, lw=1.2, label=f'OpenStat {k}')
    if include_other:
        for k, d in dev_other.items():
            ax.plot(d.E_V, d.dI_nA, color='crimson', ls='--', lw=1.2, label=f'OpenStat {k} (reverse scan)')
    psaxis(ax); ax.set_ylabel('ΔI [nA]')

fig, ax = plt.subplots(2, 1, figsize=(12, 10))
overlay(ax[0]); ax[0].set_title('All runs, OpenStat × −1 (full range)')
ax[0].legend(fontsize=8, ncol=2)
overlay(ax[1]); ax[1].set_ylim(-560, -150); ax[1].set_title('Same, zoomed')
plt.tight_layout()
plt.show()

# ## 4. Mean ± spread of each instrument, and the difference
#
# OpenStat runs are interpolated onto the PalmSens potential grid (the two step sizes differ slightly:
# ~5.035 mV vs ~4.997 mV). `sample_81` is excluded because it is a different scan direction and interval.

E = next(iter(ps.values())).E_V.values                       # common grid = PalmSens potentials
E = E[(E >= max(d.E_V.min() for d in dev.values())) & (E <= min(d.E_V.max() for d in dev.values()))]   # overlap only

def on_grid(d):
    s = d.sort_values('E_V')
    return np.interp(E, s.E_V, s.dI_nA)

P = np.vstack([on_grid(d) for d in ps.values()])
D = np.vstack([on_grid(d) for d in dev_same.values()])
p_mean, p_sd = np.nanmean(P, 0), np.nanstd(P, 0)
d_mean, d_sd = np.nanmean(D, 0), np.nanstd(D, 0)
diff = d_mean - p_mean

fig, ax = plt.subplots(2, 1, figsize=(12, 8.5), sharex=True, height_ratios=[2, 1])
ax[0].plot(E, p_mean, color='tab:orange', lw=2, label=f'PalmSens mean (n={len(P)})')
ax[0].fill_between(E, p_mean - p_sd, p_mean + p_sd, color='tab:orange', alpha=0.25, label='±1 SD')
ax[0].plot(E, d_mean, color='tab:blue', lw=2, label=f'OpenStat mean, flipped (n={len(D)})')
ax[0].fill_between(E, d_mean - d_sd, d_mean + d_sd, color='tab:blue', alpha=0.25, label='±1 SD')
ax[0].set_ylim(-560, -150); ax[0].set_ylabel('ΔI [nA]'); ax[0].legend(fontsize=9)
ax[0].set_title('Mean of repeats')

ax[1].axhline(0, color='k', lw=0.8)
ax[1].plot(E, diff, color='tab:purple')
ax[1].axvspan(0.62, TRANSIENT_CUTOFF_V, color='grey', alpha=0.15, label='OpenStat start transient')
ax[1].set_ylim(-150, 150); ax[1].set_ylabel('OpenStat − PalmSens [nA]'); ax[1].legend(fontsize=9)
psaxis(ax[1])
plt.tight_layout()
plt.show()

# ## 5. Shape only — offset and scale removed
#
# Each mean curve has its own mean subtracted and is divided by its own standard deviation (computed below
# the transient cutoff), so this shows whether the *features* line up regardless of gain/offset error.

mask = (E < TRANSIENT_CUTOFF_V) & np.isfinite(d_mean)
z = lambda y: (y - y[mask].mean()) / y[mask].std()

fig, ax = plt.subplots(figsize=(12, 4.5))
ax.plot(E[mask], z(p_mean)[mask], color='tab:orange', lw=2, label='PalmSens mean')
ax.plot(E[mask], z(d_mean)[mask], color='tab:blue', lw=2, label='OpenStat mean (flipped)')
for k, d in ps.items():
    ax.plot(E[mask], z(on_grid(d))[mask], color='tab:orange', lw=0.7, alpha=0.5)
ax.set_ylabel('normalised ΔI [z-score]'); ax.set_title(f'Shape comparison, E < {TRANSIENT_CUTOFF_V} V')
psaxis(ax); ax.set_xlim(TRANSIENT_CUTOFF_V + 0.01, -0.22); ax.legend()
plt.tight_layout()
plt.show()

# ## 6. Numbers

def stats(y, ref, m):
    ok = m & np.isfinite(y) & np.isfinite(ref)
    e = y[ok] - ref[ok]
    return {'mean ΔI [nA]': y[ok].mean(), 'PalmSens mean [nA]': ref[ok].mean(),
            'offset [nA]': e.mean(), 'RMSE [nA]': np.sqrt((e ** 2).mean()),
            'ratio of means': y[ok].mean() / ref[ok].mean(), 'Pearson r': np.corrcoef(y[ok], ref[ok])[0, 1]}

full = np.ones_like(E, bool)
rows = {}
for k, d in {**dev_same, **dev_other}.items():
    y = on_grid(d)
    tag = k + (' (reverse scan)' if k in dev_other else '')
    rows[(tag, 'full range')] = stats(y, p_mean, full)
    rows[(tag, f'E < {TRANSIENT_CUTOFF_V} V')] = stats(y, p_mean, E < TRANSIENT_CUTOFF_V)
rows[('OpenStat mean', 'full range')] = stats(d_mean, p_mean, full)
rows[('OpenStat mean', f'E < {TRANSIENT_CUTOFF_V} V')] = stats(d_mean, p_mean, E < TRANSIENT_CUTOFF_V)
table = pd.DataFrame(rows).T.round(3)
print(
    table
)

# repeatability: how far apart are repeats on the SAME instrument?
lo = E < TRANSIENT_CUTOFF_V
print(
    pd.DataFrame({
        'PalmSens': {'runs': len(P), 'mean SD between runs [nA]': np.nanmean(p_sd[lo]),
                     'spread of run means [nA]': np.ptp(np.nanmean(P[:, lo], 1)),
                     'point-to-point noise [nA]': np.mean([np.std(np.diff(r[lo], 2)) / np.sqrt(6) for r in P])},
        'OpenStat': {'runs': len(D), 'mean SD between runs [nA]': np.nanmean(d_sd[lo]),
                     'spread of run means [nA]': np.ptp(np.nanmean(D[:, lo & np.isfinite(d_mean)], 1)),
                     'point-to-point noise [nA]': np.mean([np.std(np.diff(r[lo & np.isfinite(r)], 2)) / np.sqrt(6) for r in D])},
    }).round(2)
)

# ## 7. OpenStat only — the two samples behind each ΔI
#
# PalmSens only stores the difference; the OpenStat CSV also keeps `I_before` and `I_end`. Shown as
# recorded (not flipped). The large values at 0.6 V are the start-of-scan transient.

fig, ax = plt.subplots(1, 2, figsize=(13, 4.5))
for (k, d), c in zip(dev_same.items(), dev_colors):
    ax[0].plot(d.E_V, d.I_before_nA, color=c, label=k)
    ax[0].plot(d.E_V, d.I_end_nA, color=c, ls='--')
    ax[1].plot(d.t_s, d.dI_nA, color=c, label=k)
ax[0].set_ylim(-400, 800); ax[0].set_ylabel('I [nA]'); psaxis(ax[0])
ax[0].set_title('I_before (solid) and I_end (dashed), as recorded'); ax[0].legend(fontsize=8)
for (k, d), c in zip(ps.items(), ps_colors):
    ax[1].plot(d.t_s, d.dI_nA, color=c, lw=2, label=f'PalmSens {k}')
ax[1].set_ylim(-560, -150); ax[1].set_xlabel('time since first point [s]'); ax[1].set_ylabel('ΔI [nA]')
ax[1].set_title('ΔI vs time (OpenStat flipped)'); ax[1].legend(fontsize=7, ncol=2)
plt.tight_layout()
plt.show()

# ## 8. What the comparison shows (values from the run saved in this notebook)
#
# - **Sign:** OpenStat `dI_nA` has the opposite sign to PalmSens on every run. After × −1 both sit in the
#   same −170 … −500 nA band, so the magnitude is in the right ballpark.
# - **Level:** below 0.45 V the OpenStat mean is about −204 nA against −288 nA for PalmSens, i.e. roughly
#   70 % of the PalmSens magnitude (offset ≈ +84 nA). The PalmSens runs themselves drift by ~83 nA between
#   Salt_1 and Salt_3, so part of that gap is within the reference's own run-to-run spread.
# - **Shape does not match yet:** Pearson r between the two means is ≈ 0 below 0.45 V. PalmSens has its
#   broad maximum near 0.35 V and a clear dip around −0.05 V; OpenStat has its maximum near 0.2 V and is
#   almost flat through −0.05 V.
# - **Start of scan:** OpenStat begins at −750 … −960 nA and decays over the first ~0.15 V. PalmSens holds
#   5 s at the start potential (`T_EQUIL`) before sampling and starts at only −290 … −520 nA.
# - **Repeatability:** OpenStat repeats are much tighter than PalmSens (≈ 7 nA vs ≈ 34 nA SD between runs,
#   ≈ 0.8 nA vs ≈ 4.7 nA point-to-point), with a slow monotonic drift from sample_82 to sample_88.
# - **`sample_81`** (−0.2 → 0.6 V, 0.5 s interval) is the only OpenStat run with a dip near 0 V, but it is a
#   different method from the PalmSens runs, so it is shown for reference only.
# - **Files:** `Salt_4` is an identical copy of `Salt_3`, and `sample_83 (1)` of `sample_83`.
# - The PalmSens values used here are the raw `DataSet` currents, not the smoothed curve PSTrace displays
#   (`SMOOTH_LEVEL=1`), so they look slightly noisier than on the PSTrace screen.

# ## 9. `Salt_3_2` (originally sample_90) vs PalmSens
#
# `Salt_3_2` (originally sample_90) on its own against the three PalmSens runs, with the earlier OpenStat mean (samples 82-86, Salt_2_1, Salt_2_2)
# in grey for reference. Same flips as above (× −1, nA, PSTrace x direction).

f = dev[FOCUS]
fy = on_grid(f)

fig, ax = plt.subplots(2, 2, figsize=(14, 9))
a = ax[0, 0]
for (k, d), c in zip(ps.items(), ps_colors):
    a.plot(d.E_V, d.dI_nA, color=c, lw=1.5, label=f'PalmSens {k}')
a.plot(E, d_mean, color='grey', lw=1.5, ls=':', label='OpenStat mean 82-88')
a.plot(f.E_V, f.dI_nA, color='tab:blue', lw=2.2, label=f'OpenStat {FOCUS}')
a.set_ylim(-560, -150); a.set_ylabel('ΔI [nA]'); a.set_title(f'{FOCUS} (flipped) vs PalmSens'); a.legend(fontsize=8); psaxis(a)

a = ax[0, 1]
a.axhline(0, color='k', lw=0.8)
a.plot(E, fy - p_mean, color='tab:purple', label=f'{FOCUS} − PalmSens mean')
a.plot(E, d_mean - p_mean, color='grey', ls=':', label='mean 82-88 − PalmSens mean')
a.fill_between(E, -p_sd, p_sd, color='tab:orange', alpha=0.2, label='PalmSens ±1 SD')
a.set_ylim(-150, 200); a.set_ylabel('difference [nA]'); a.set_title('Difference'); a.legend(fontsize=8); psaxis(a)

a = ax[1, 0]
a.plot(E[mask], z(p_mean)[mask], color='tab:orange', lw=2, label='PalmSens mean')
a.plot(E[mask], z(fy)[mask], color='tab:blue', lw=2, label=FOCUS)
a.plot(E[mask], z(d_mean)[mask], color='grey', ls=':', label='OpenStat mean 82-88')
a.set_ylabel('normalised ΔI [z-score]'); a.set_title(f'Shape only, E < {TRANSIENT_CUTOFF_V} V'); a.legend(fontsize=8)
psaxis(a); a.set_xlim(TRANSIENT_CUTOFF_V + 0.01, -0.22)

a = ax[1, 1]
a.plot(f.E_V, f.I_before_nA, color='tab:blue', label=f'{FOCUS} I_before')
a.plot(f.E_V, f.I_end_nA, color='tab:blue', ls='--', label=f'{FOCUS} I_end')
ref = dev_same[list(dev_same)[-1]]
a.plot(ref.E_V, ref.I_before_nA, color='grey', lw=1, label=f'{list(dev_same)[-1]} I_before')
a.plot(ref.E_V, ref.I_end_nA, color='grey', lw=1, ls='--', label=f'{list(dev_same)[-1]} I_end')
a.set_ylim(-400, 800); a.set_ylabel('I [nA]'); a.set_title('Raw samples, as recorded'); a.legend(fontsize=8); psaxis(a)
plt.tight_layout()
plt.show()

rows = {}
for label, y in [(FOCUS, fy), ('OpenStat mean 82-88', d_mean)]:
    rows[(label, 'full range')] = stats(y, p_mean, full)
    rows[(label, f'E < {TRANSIENT_CUTOFF_V} V')] = stats(y, p_mean, E < TRANSIENT_CUTOFF_V)
for k, d in ps.items():                                   # how well does each PalmSens run match the PalmSens mean?
    rows[(f'PalmSens {k}', f'E < {TRANSIENT_CUTOFF_V} V')] = stats(on_grid(d), p_mean, E < TRANSIENT_CUTOFF_V)
print(f'{FOCUS}: RTIA code {f.rtia_code.unique().tolist()}, interval {f.t_s.diff().mean():.4f} s, '
      f'point-to-point noise {np.std(np.diff(fy[lo], 2)) / np.sqrt(6):.2f} nA')
print(
    pd.DataFrame(rows).T.round(3)
)

# **`Salt_3_2` (originally sample_90) in short** (values from the run saved here)
#
# - Below 0.45 V it averages −223 nA against −288 nA for PalmSens: about 78 % of the PalmSens magnitude,
#   up from about 71 % for samples 82–88 (offset +64 nA instead of +84 nA).
# - The shape is essentially the same as samples 82–88: broad maximum near 0.2 V instead of 0.35 V, no dip
#   at −0.05 V, and the same large start-of-scan decay from about −920 nA. Pearson r against the PalmSens
#   mean is −0.36 below 0.45 V.
# - It ran on RTIA code 5 (like samples 82–83), and is the smoothest OpenStat run so far (≈ 0.5 nA
#   point-to-point), apart from a kink in the last two points near −0.19 V.
