"""Exported from Best_FC_Comparison.ipynb.

Usage: python best_fc_comparison.py [data folder containing "Palmsens" and "My Device"]
"""
import os
import sys

os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')


# # Best FC comparison — OpenStat vs PalmSens
#
# Correlates every OpenStat `FC_*` run against every PalmSens `FC_*` run, then picks the **3 best-correlated
# pairs** (3 OpenStat files + 3 PalmSens files, each file used once) and plots them.
#
# Change the settings in the first cell, then *Run All*.

# ----------------------------- SETTINGS -----------------------------
PALMSENS_GLOB = 'Palmsens/FC_*.pssession'
MY_GLOB       = 'My Device/FC_*.csv'

N_BEST          = 3         # how many pairs to pick
SAME_LEVEL_ONLY = True      # only pair FC_N_x with FC_N_y (same N on both instruments)
ONE_PER_LEVEL   = True      # at most one pair per FC level N
CORR_RANGE_V    = None      # potential window for the correlation, e.g. (-0.34, 0.45); None = full overlap

FLIP_MY_CURRENT = True      # multiply my dI by -1 so the sign matches PalmSens
FLIP_X_AXIS     = True      # draw 0.6 V on the left, like PSTrace
MY_COLUMN       = 'dI_nA'   # 'dI_nA', 'I_before_nA' or 'I_end_nA'
UNIT            = 'uA'      # 'uA' or 'nA' for the plots
SAVE_AS         = None      # e.g. 'best_fc.png', or None to not save
# ---------------------------------------------------------------------

import json
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

SCALE = 1e-3 if UNIT == 'uA' else 1.0
YLABEL = f"{'ΔI' if MY_COLUMN == 'dI_nA' else 'I'} [{'µA' if UNIT == 'uA' else 'nA'}]"


def load_palmsens(path):
    # .pssession = UTF-16 JSON; current is stored in uA -> returned in nA
    raw = Path(path).read_bytes().decode('utf-16').strip('﻿\x00 \r\n')
    values = json.loads(raw)['Measurements'][0]['DataSet']['Values']
    arrays = {a['Description']: np.array([p['V'] for p in a['DataValues']]) for a in values}
    return arrays['potential'], arrays['current'] * 1e3


def load_mine(path):
    df = pd.read_csv(path)
    sign = -1 if FLIP_MY_CURRENT else 1
    return df['E_applied_V'].values, sign * df[MY_COLUMN].values


def drop_copies(runs):
    # a file whose data is identical to an earlier-named one is a copy - keep the first only
    kept, copies = {}, []
    for name, (E, I) in runs.items():
        twin = next((k for k, (_, I2) in kept.items() if len(I2) == len(I) and np.array_equal(I2, I)), None)
        if twin is None:
            kept[name] = (E, I)
        else:
            copies.append(f'{name} is identical to {twin}')
    return kept, copies


def level(name):                                            # 'FC_3_2' -> '3'
    return name.split('_')[1]


ps, ps_copies = drop_copies({p.stem: load_palmsens(p) for p in sorted(Path('.').glob(PALMSENS_GLOB))})
my, my_copies = drop_copies({p.stem: load_mine(p) for p in sorted(Path('.').glob(MY_GLOB))})
print('PalmSens runs:', list(ps))
print('OpenStat runs:', list(my))
print(
    print('Copies left out -> PalmSens:', ps_copies, '| OpenStat:', my_copies)
)

# ## 1. Correlation of every OpenStat run against every PalmSens run

# common potential grid = the range every run covers
lo = max(E.min() for E, _ in list(ps.values()) + list(my.values()))
hi = min(E.max() for E, _ in list(ps.values()) + list(my.values()))
if CORR_RANGE_V:
    lo, hi = max(lo, min(CORR_RANGE_V)), min(hi, max(CORR_RANGE_V))
GRID = np.arange(lo, hi + 1e-9, 0.005)

def on_grid(run):
    E, I = run
    order = np.argsort(E)
    return np.interp(GRID, E[order], I[order])

corr = pd.DataFrame({p: {m: np.corrcoef(on_grid(my[m]), on_grid(ps[p]))[0, 1] for m in my} for p in ps})
corr.index.name, corr.columns.name = 'OpenStat', 'PalmSens'

# rank the allowed pairs and take the best ones, never re-using a file
ranked = corr.stack().sort_values(ascending=False)
best, used_my, used_ps, used_levels = [], set(), set(), set()
for (m, p), r in ranked.items():
    if SAME_LEVEL_ONLY and level(m) != level(p):
        continue
    if m in used_my or p in used_ps or (ONE_PER_LEVEL and level(m) in used_levels):
        continue
    best.append((m, p, r)); used_my.add(m); used_ps.add(p); used_levels.add(level(m))
    if len(best) == N_BEST:
        break

print(f'Correlation window: {lo:.3f} V to {hi:.3f} V ({len(GRID)} points)')
for m, p, r in best:
    print(f'  OpenStat {m}  <->  PalmSens {p}   r = {r:.4f}')

fig, ax = plt.subplots(figsize=(10, 7), dpi=110)
im = ax.imshow(corr.values, cmap='viridis', vmin=max(0, np.nanmin(corr.values)), vmax=1, aspect='auto')
ax.set_xticks(range(len(corr.columns)), corr.columns, rotation=45, ha='right')
ax.set_yticks(range(len(corr.index)), corr.index)
ax.set_xlabel('PalmSens'); ax.set_ylabel('OpenStat')
for i in range(corr.shape[0]):
    for j in range(corr.shape[1]):
        v = corr.values[i, j]
        ax.text(j, i, f'{v:.2f}', ha='center', va='center', fontsize=7, color='white' if v < 0.8 else 'black')
for m, p, _ in best:
    i, j = corr.index.get_loc(m), corr.columns.get_loc(p)
    ax.add_patch(plt.Rectangle((j - 0.5, i - 0.5), 1, 1, fill=False, ec='red', lw=2.5))
ax.set_title('Pearson r (red box = selected pair)')
fig.colorbar(im, label='r'); plt.tight_layout()
plt.show()

# ## 2. The selected pairs

def finish(ax):
    if FLIP_X_AXIS:
        ax.invert_xaxis()
    ax.set_xlabel('E [V]'); ax.set_ylabel(YLABEL); ax.grid(alpha=0.3); ax.legend(fontsize=8)

colors = plt.cm.tab10(range(len(best)))
fig, ax = plt.subplots(1, len(best), figsize=(6 * len(best), 5), dpi=110, squeeze=False)
for a, (m, p, r) in zip(ax[0], best):
    a.plot(ps[p][0], ps[p][1] * SCALE, color='tab:orange', lw=2, label=f'PalmSens {p}')
    a.plot(my[m][0], my[m][1] * SCALE, color='tab:blue', lw=2,
           label=f'OpenStat {m}' + (' (×−1)' if FLIP_MY_CURRENT else ''))
    a.set_title(f'{m} vs {p}   r = {r:.3f}'); finish(a)
plt.tight_layout()
if SAVE_AS:
    fig.savefig(SAVE_AS, dpi=200)
plt.show()

fig, ax = plt.subplots(figsize=(12, 6.5), dpi=110)
for (m, p, r), c in zip(best, colors):
    ax.plot(ps[p][0], ps[p][1] * SCALE, color=c, lw=2, ls='--', label=f'PalmSens {p}')
    ax.plot(my[m][0], my[m][1] * SCALE, color=c, lw=2, label=f'OpenStat {m}  (r = {r:.3f})')
ax.set_title('Best pairs together — solid = OpenStat, dashed = PalmSens'); finish(ax)
plt.tight_layout()
plt.show()

# ## 3. Numbers for the selected pairs
#
# Peak = largest |current| away from the start of the scan (E < 0.45 V), so the start-of-scan transient
# is not picked up as the peak.

def peak(run):
    E, I = run
    keep = E < 0.45
    i = np.abs(I[keep]).argmax()
    return E[keep][i], I[keep][i]

rows = []
for m, p, r in best:
    (Em, Im), (Ep, Ip) = peak(my[m]), peak(ps[p])
    ym, yp = on_grid(my[m]), on_grid(ps[p])
    rows.append({'OpenStat': m, 'PalmSens': p, 'Pearson r': round(r, 4),
                 'OpenStat peak [µA]': round(Im / 1e3, 2), 'PalmSens peak [µA]': round(Ip / 1e3, 2),
                 'peak ratio (Open/Palm)': round(Im / Ip, 3),
                 'OpenStat peak E [V]': round(Em, 3), 'PalmSens peak E [V]': round(Ep, 3),
                 'peak shift [mV]': round((Em - Ep) * 1e3, 1),
                 'RMSE [µA]': round(np.sqrt(np.mean((ym - yp) ** 2)) / 1e3, 2)})
print(
    pd.DataFrame(rows).set_index(['OpenStat', 'PalmSens'])
)

# Full correlation table, for reference:

print(
    corr.round(3)
)
