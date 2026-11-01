"""Exported from Best_Salt_Comparison.ipynb.

Usage: python best_salt_comparison.py [data folder containing "Palmsens" and "My Device"]
"""
import os
import sys

os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')


# # Best Salt comparison — OpenStat vs PalmSens
#
# Compares the chosen PalmSens salt runs against the chosen OpenStat salt runs: all of them
# overlaid, each OpenStat run paired with a PalmSens run, and the numbers.
#
# Change the settings in the first cell, then *Run All*.

# ----------------------------- SETTINGS -----------------------------
PALMSENS_FILES = ['Palmsens/Salt_5_1.pssession',    # the PalmSens runs to compare
                  'Palmsens/Salt_2_1.pssession',
                  'Palmsens/Salt_2_2.pssession',
                  'Palmsens/Salt_4_1.pssession',    # added
                  'Palmsens/Salt_5_2.pssession']    # added
MY_FILES       = ['My Device/Salt_5_1.csv',         # the OpenStat runs to compare
                  'My Device/Salt_5_2.csv',
                  'My Device/Salt_5_3.csv',
                  'My Device/Salt_2_1.csv',         # added
                  'My Device/Salt_2_2.csv']         # added

CORR_RANGE_V       = None   # potential window for the correlation, e.g. (-0.2, 0.45); None = full overlap
TRANSIENT_CUTOFF_V = 0.45   # the table also reports values below this, without the start-of-scan decay

FLIP_MY_CURRENT = True      # multiply my dI by -1 so the sign matches PalmSens
FLIP_X_AXIS     = True      # draw 0.6 V on the left, like PSTrace
MY_COLUMN       = 'dI_nA'   # 'dI_nA', 'I_before_nA' or 'I_end_nA'
UNIT            = 'nA'      # 'uA' or 'nA' for the plots
SAVE_AS         = None      # e.g. 'best_salt.png', or None to not save

# every salt run on each instrument - only used for the reference table at the end
ALL_PALMSENS_GLOB = 'Palmsens/Salt_?_?.pssession'
ALL_MY_GLOB       = 'My Device/Salt_?_?.csv'
# ---------------------------------------------------------------------

import json
from itertools import permutations
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


def finish(ax):
    if FLIP_X_AXIS:
        ax.invert_xaxis()
    ax.set_xlabel('E [V]'); ax.set_ylabel(YLABEL); ax.grid(alpha=0.3); ax.legend(fontsize=8)


ps = {Path(f).stem: load_palmsens(f) for f in PALMSENS_FILES}
my = {Path(f).stem: load_mine(f) for f in MY_FILES}
print('PalmSens runs:', list(ps))
print(
    print('OpenStat runs:', list(my))
)

# ## 1. Correlation between the chosen runs

# common potential grid = the range every run covers
lo = max(E.min() for E, _ in list(ps.values()) + list(my.values()))
hi = min(E.max() for E, _ in list(ps.values()) + list(my.values()))
if CORR_RANGE_V:
    lo, hi = max(lo, min(CORR_RANGE_V)), min(hi, max(CORR_RANGE_V))
GRID = np.arange(lo, hi + 1e-9, 0.005)
LOW = GRID < TRANSIENT_CUTOFF_V

def on_grid(run):
    E, I = run
    order = np.argsort(E)
    return np.interp(GRID, E[order], I[order])

corr = pd.DataFrame({p: {m: np.corrcoef(on_grid(my[m]), on_grid(ps[p]))[0, 1] for m in my} for p in ps})
corr.index.name, corr.columns.name = 'OpenStat', 'PalmSens'

# one-to-one pairing of the chosen runs that gives the highest total correlation
order = max(permutations(corr.columns, len(corr.index)),
            key=lambda cols: sum(corr.loc[m, p] for m, p in zip(corr.index, cols)))
best = [(m, p, corr.loc[m, p]) for m, p in zip(corr.index, order)]

print(f'Correlation window: {lo:.3f} V to {hi:.3f} V ({len(GRID)} points)')
for m, p, r in best:
    print(f'  OpenStat {m}  <->  PalmSens {p}   r = {r:.4f}')

fig, ax = plt.subplots(figsize=(8, 5.5), dpi=110)
im = ax.imshow(corr.values, cmap='viridis', aspect='auto')
ax.set_xticks(range(len(corr.columns)), corr.columns)
ax.set_yticks(range(len(corr.index)), corr.index)
ax.set_xlabel('PalmSens'); ax.set_ylabel('OpenStat')
mid = (corr.values.min() + corr.values.max()) / 2
for i in range(corr.shape[0]):
    for j in range(corr.shape[1]):
        v = corr.values[i, j]
        ax.text(j, i, f'{v:.3f}', ha='center', va='center', fontsize=11, color='white' if v < mid else 'black')
for m, p, _ in best:
    i, j = corr.index.get_loc(m), corr.columns.get_loc(p)
    ax.add_patch(plt.Rectangle((j - 0.5, i - 0.5), 1, 1, fill=False, ec='red', lw=2.5))
ax.set_title('Pearson r (red box = pairing used below)')
fig.colorbar(im, label='r'); plt.tight_layout()
plt.show()

# ## 2. All chosen runs together

fig, ax = plt.subplots(2, 1, figsize=(12, 10), dpi=110)
for a in ax:
    for (k, (E, I)), c in zip(ps.items(), plt.cm.Oranges(np.linspace(0.45, 0.9, len(ps)))):
        a.plot(E, I * SCALE, color=c, lw=2, label=f'PalmSens {k}')
    for (k, (E, I)), c in zip(my.items(), plt.cm.Blues(np.linspace(0.5, 0.95, len(my)))):
        a.plot(E, I * SCALE, color=c, lw=2, label=f'OpenStat {k}' + (' (×−1)' if FLIP_MY_CURRENT else ''))
    finish(a)
ax[0].set_title('Full range')
lows = [on_grid(r)[LOW].min() for r in list(ps.values()) + list(my.values())]
highs = [on_grid(r)[LOW].max() for r in list(ps.values()) + list(my.values())]
pad = 0.08 * (max(highs) - min(lows))
ax[1].set_ylim((min(lows) - pad) * SCALE, (max(highs) + pad) * SCALE)
ax[1].set_title(f'Zoomed to the levels below {TRANSIENT_CUTOFF_V} V')
plt.tight_layout()
plt.show()

# ## 3. Pair by pair

ncol = min(3, len(best)); nrow = -(-len(best) // ncol)
fig, ax = plt.subplots(nrow, ncol, figsize=(6 * ncol, 5 * nrow), dpi=110, squeeze=False)
for a in ax.flat[len(best):]:
    a.axis('off')
for a, (m, p, r) in zip(ax.flat, best):
    a.plot(ps[p][0], ps[p][1] * SCALE, color='tab:orange', lw=2, label=f'PalmSens {p}')
    a.plot(my[m][0], my[m][1] * SCALE, color='tab:blue', lw=2,
           label=f'OpenStat {m}' + (' (×−1)' if FLIP_MY_CURRENT else ''))
    a.set_title(f'{m} vs {p}   r = {r:.3f}'); finish(a)
plt.tight_layout()
plt.show()

colors = plt.cm.tab10(range(len(best)))
fig, ax = plt.subplots(2, 1, figsize=(12, 11), dpi=110)
for a in ax:
    for (m, p, r), c in zip(best, colors):
        a.plot(ps[p][0], ps[p][1] * SCALE, color=c, lw=2, ls='--', label=f'PalmSens {p}')
        a.plot(my[m][0], my[m][1] * SCALE, color=c, lw=2, label=f'OpenStat {m}  (r = {r:.3f})')
    finish(a)
ax[0].set_title('Best pairs together — solid = OpenStat, dashed = PalmSens')
lows = [on_grid(r)[LOW].min() for r in list(ps.values()) + list(my.values())]
highs = [on_grid(r)[LOW].max() for r in list(ps.values()) + list(my.values())]
pad = 0.08 * (max(highs) - min(lows))
ax[1].set_ylim((min(lows) - pad) * SCALE, (max(highs) + pad) * SCALE)
ax[1].set_title(f'Same, zoomed to the levels below {TRANSIENT_CUTOFF_V} V')
plt.tight_layout()
if SAVE_AS:
    fig.savefig(SAVE_AS, dpi=200)
plt.show()

# ## 4. Numbers for each pair
#
# The salt scans have no analyte peak, so the comparison is on level and shape. "low" rows use only
# E < `TRANSIENT_CUTOFF_V`, which leaves out the start-of-scan decay that dominates the full-range values.

rows = []
for m, p, r in best:
    ym, yp = on_grid(my[m]), on_grid(ps[p])
    rows.append({'OpenStat': m, 'PalmSens': p,
                 'r (full)': round(r, 3),
                 'r (low)': round(np.corrcoef(ym[LOW], yp[LOW])[0, 1], 3),
                 'OpenStat mean, low [nA]': round(ym[LOW].mean(), 1),
                 'PalmSens mean, low [nA]': round(yp[LOW].mean(), 1),
                 'ratio (Open/Palm), low': round(ym[LOW].mean() / yp[LOW].mean(), 3),
                 'offset, low [nA]': round((ym - yp)[LOW].mean(), 1),
                 'RMSE, low [nA]': round(np.sqrt(np.mean((ym - yp)[LOW] ** 2)), 1),
                 'RMSE, full [nA]': round(np.sqrt(np.mean((ym - yp) ** 2)), 1),
                 'OpenStat first point [nA]': round(my[m][1][0], 0),
                 'PalmSens first point [nA]': round(ps[p][1][0], 0)})
print(
    pd.DataFrame(rows).set_index(['OpenStat', 'PalmSens']).T
)

# ## 5. Reference — every salt run against every salt run
#
# The same correlation over all `Salt_N_M` files on both instruments, so the chosen six can be checked
# against the alternatives. Top: full range. Bottom: only E < `TRANSIENT_CUTOFF_V`.

all_ps = {p.stem: load_palmsens(p) for p in sorted(Path('.').glob(ALL_PALMSENS_GLOB))}
all_my = {p.stem: load_mine(p) for p in sorted(Path('.').glob(ALL_MY_GLOB))}

def corr_table(sel):
    t = pd.DataFrame({p: {m: np.corrcoef(on_grid(all_my[m])[sel], on_grid(all_ps[p])[sel])[0, 1]
                          for m in all_my} for p in all_ps})
    t.index.name, t.columns.name = 'OpenStat', 'PalmSens'
    return t

full_tab, low_tab = corr_table(np.ones_like(GRID, bool)), corr_table(LOW)
fig, ax = plt.subplots(2, 1, figsize=(10, 9.5), dpi=110)
for a, t, title in [(ax[0], full_tab, 'Pearson r, full range'),
                    (ax[1], low_tab, f'Pearson r, E < {TRANSIENT_CUTOFF_V} V')]:
    im = a.imshow(t.values, cmap='viridis', aspect='auto')
    a.set_xticks(range(len(t.columns)), t.columns, rotation=45, ha='right')
    a.set_yticks(range(len(t.index)), t.index)
    a.set_xlabel('PalmSens'); a.set_ylabel('OpenStat'); a.set_title(title + ' (red box = chosen runs)')
    mid = (t.values.min() + t.values.max()) / 2
    for i in range(t.shape[0]):
        for j in range(t.shape[1]):
            a.text(j, i, f'{t.values[i, j]:.2f}', ha='center', va='center', fontsize=8,
                   color='white' if t.values[i, j] < mid else 'black')
            if t.index[i] in my and t.columns[j] in ps:
                a.add_patch(plt.Rectangle((j - 0.5, i - 0.5), 1, 1, fill=False, ec='red', lw=2))
    fig.colorbar(im, ax=a, label='r')
plt.tight_layout()
plt.show()
