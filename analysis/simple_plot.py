"""Exported from simple_plot.ipynb.

Usage: python simple_plot.py [data folder containing "Palmsens" and "My Device"]
"""
import os
import sys

os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')


# # PalmSens vs OpenStat — simple plot
#
# Change the settings in the first cell, then *Run All*.

# ----------------------------- Salt Testing -----------------------------
PALMSENS_FILES = ['Palmsens/Salt_2_1.pssession',
                  'Palmsens/Salt_2_2.pssession']
MY_FILES       = ['My Device/Salt_2_1.csv',
                  'My Device/Salt_2_2.csv']  
FLIP_MY_CURRENT = True      # multiply my dI by -1 so the sign matches PalmSens
FLIP_X_AXIS     = True      # draw 0.6 V on the left, like PSTrace
MY_COLUMN       = 'dI_nA'   # 'dI_nA', 'I_before_nA' or 'I_end_nA'

Y_LIMITS = None             # nA, e.g. (-560, -150), or None for automatic
X_LIMITS = None             # e.g. (0.6, -0.2), or None for automatic
TITLE    = 'DPV: PalmSens vs OpenStat'
SAVE_AS  = None             # e.g. 'comparison.png', or None to not save
# ---------------------------------------------------------------------

# ----------------------------- Salt 5 -----------------------------
PALMSENS_FILES = ['Palmsens/Salt_4_1.pssession',
                  'Palmsens/Salt_5_1.pssession',
                  'Palmsens/Salt_5_2.pssession']
MY_FILES       = ['My Device/Salt_5_1.csv',
                  'My Device/Salt_5_2.csv',
                  'My Device/Salt_5_3.csv']  
FLIP_MY_CURRENT = True      # multiply my dI by -1 so the sign matches PalmSens
FLIP_X_AXIS     = True      # draw 0.6 V on the left, like PSTrace
MY_COLUMN       = 'dI_nA'   # 'dI_nA', 'I_before_nA' or 'I_end_nA'

Y_LIMITS = None             # nA, e.g. (-560, -150), or None for automatic
X_LIMITS = None             # e.g. (0.6, -0.2), or None for automatic
TITLE    = 'DPV: PalmSens vs OpenStat'
SAVE_AS  = None             # e.g. 'comparison.png', or None to not save
# ---------------------------------------------------------------------

# ----------------------------- Combined Best Salt Comparrison -----------------------------
PALMSENS_FILES = ['Palmsens/Salt_5_1.pssession',
                  'Palmsens/Salt_2_1.pssession',
                  'Palmsens/Salt_2_2.pssession']

MY_FILES       = ['My Device/Salt_5_1.csv',
                  'My Device/Salt_5_2.csv',
                  'My Device/Salt_5_3.csv'] 
FLIP_MY_CURRENT = True      # multiply my dI by -1 so the sign matches PalmSens
FLIP_X_AXIS     = True      # draw 0.6 V on the left, like PSTrace
MY_COLUMN       = 'dI_nA'   # 'dI_nA', 'I_before_nA' or 'I_end_nA'

Y_LIMITS = None             # nA, e.g. (-560, -150), or None for automatic
X_LIMITS = None             # e.g. (0.6, -0.2), or None for automatic
TITLE    = 'DPV: PalmSens vs OpenStat'
SAVE_AS  = None             # e.g. 'comparison.png', or None to not save
# ---------------------------------------------------------------------

import json
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt


def load_palmsens(path):
    # .pssession = UTF-16 JSON; current is stored in uA -> returned in nA
    raw = Path(path).read_bytes().decode('utf-16').strip('\ufeff\x00 \r\n')
    values = json.loads(raw)['Measurements'][0]['DataSet']['Values']
    arrays = {a['Description']: np.array([p['V'] for p in a['DataValues']]) for a in values}
    return arrays['potential'], arrays['current'] * 1e3


def load_mine(path):
    df = pd.read_csv(path)
    sign = -1 if FLIP_MY_CURRENT else 1
    return df['E_applied_V'].values, sign * df[MY_COLUMN].values


def overall_error(E_ref, I_ref, E, I):
    # OpenStat vs PalmSens over the potential range both cover; PalmSens is
    # interpolated onto the OpenStat potentials (np.interp needs ascending x)
    order = np.argsort(E_ref)
    E_ref, I_ref = E_ref[order], I_ref[order]
    mask = (E >= E_ref[0]) & (E <= E_ref[-1])
    ref = np.interp(E[mask], E_ref, I_ref)
    diff = I[mask] - ref
    rmse = np.sqrt(np.mean(diff ** 2))
    mae = np.mean(np.abs(diff))
    pct = 100 * mae / np.mean(np.abs(ref))      # mean abs error as % of mean |PalmSens|
    return rmse, mae, pct, mask.sum()


fig, ax = plt.subplots(figsize=(11, 6), dpi=110)

palmsens, mine = [], []

for f, c in zip(PALMSENS_FILES, plt.cm.Oranges(np.linspace(0.45, 0.9, len(PALMSENS_FILES)))):
    E, I = load_palmsens(f)
    palmsens.append((Path(f).stem, E, I))
    ax.plot(E, I, color=c, lw=1.8, label=f'PalmSens {Path(f).stem}')

for f, c in zip(MY_FILES, plt.cm.winter(np.linspace(0.1, 0.8, len(MY_FILES)))):
    E, I = load_mine(f)
    mine.append((Path(f).stem, E, I))
    ax.plot(E, I, color=c, lw=2, label=f'OpenStat {Path(f).stem}' + (' (×−1)' if FLIP_MY_CURRENT else ''))

# overall error of every OpenStat curve against every PalmSens curve
error_lines = []
for my_name, E, I in mine:
    for ps_name, E_ref, I_ref in palmsens:
        rmse, mae, pct, n = overall_error(E_ref, I_ref, E, I)
        error_lines.append(f'{my_name} vs {ps_name}: RMSE {rmse:.1f} nA, MAE {mae:.1f} nA ({pct:.1f} %)')
        print(f'{error_lines[-1]}  [{n} points]')

if FLIP_X_AXIS:
    ax.invert_xaxis()
if X_LIMITS:
    ax.set_xlim(*X_LIMITS)
if Y_LIMITS:
    ax.set_ylim(*Y_LIMITS)
ax.set_xlabel('E [V]'); ax.set_ylabel('ΔI [nA]' if MY_COLUMN == 'dI_nA' else 'I [nA]')
ax.set_title(TITLE); ax.grid(alpha=0.3); ax.legend()
if error_lines:
    ax.text(0.98, 0.03, 'Overall error\n' + '\n'.join(error_lines), transform=ax.transAxes,
            fontsize=9, va='bottom', ha='right',
            bbox=dict(boxstyle='round', facecolor='white', edgecolor='0.6', alpha=0.9))
plt.tight_layout()
if SAVE_AS:
    fig.savefig(SAVE_AS, dpi=200)
plt.show()
