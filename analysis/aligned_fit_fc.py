"""Exported from AlignedFit_FC_Comparison.ipynb.

Usage: python aligned_fit_fc.py [data folder containing "Palmsens" and "My Device"]
"""
import os
import sys

os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')


# # Aligned fit — FC, OpenStat vs PalmSens
#
# For each pair, find the **gain**, **offset** and **potential shift** that make the PalmSens curve land
# best on the OpenStat curve:
#
# $$I_\text{OpenStat}(E) \approx \text{gain} \times I_\text{PalmSens}(E - \text{shift}) + \text{offset}$$
#
# Then look at the **residual** — what still differs once those three calibratable things are removed.
#
# - gain 1.0 = same current scale; 0.8 = OpenStat reads 80 % of PalmSens
# - shift negative = OpenStat features sit at a lower potential than PalmSens
# - residual = the shape difference that a calibration cannot fix
#
# Change the settings in the first cell, then *Run All*.

# ----------------------------- SETTINGS -----------------------------
# (my OpenStat csv, PalmSens file) - one line per pair
PAIRS = [('My Device/FC_5_1.csv', 'Palmsens/FC_5_2.pssession'),
         ('My Device/FC_3_2.csv', 'Palmsens/FC_3_1.pssession'),
         ('My Device/FC_6_2.csv', 'Palmsens/FC_6_2.pssession')]

MAX_SHIFT_MV  = 80          # search shifts from -MAX to +MAX
SHIFT_STEP_MV = 0.5
FIT_RANGE_V   = None        # potential window used for the fit, e.g. (-0.2, 0.45); None = everything usable

FLIP_MY_CURRENT = True      # multiply my dI by -1 so the sign matches PalmSens
FLIP_X_AXIS     = True      # draw 0.6 V on the left, like PSTrace
MY_COLUMN       = 'dI_nA'   # 'dI_nA', 'I_before_nA' or 'I_end_nA'
UNIT            = 'uA'      # 'uA' or 'nA' for the plots and table
SAVE_AS         = None      # e.g. 'aligned_fc.png', or None to not save
# ---------------------------------------------------------------------

import json
from pathlib import Path

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

SCALE = 1e-3 if UNIT == 'uA' else 1.0
U = 'µA' if UNIT == 'uA' else 'nA'


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


def aligned_fit(mine, palm):
    # Fit  mine(E) = gain * palm(E - shift) + offset.
    # The fit window is shrunk by MAX_SHIFT at both ends so every trial shift uses the same points.
    (Em, Im), (Ep, Ip) = mine, palm
    op = np.argsort(Ep)
    Ep, Ip = Ep[op], Ip[op]
    max_s = MAX_SHIFT_MV / 1e3
    lo, hi = max(Em.min(), Ep.min()) + max_s, min(Em.max(), Ep.max()) - max_s
    if FIT_RANGE_V:
        lo, hi = max(lo, min(FIT_RANGE_V)), min(hi, max(FIT_RANGE_V))
    use = (Em >= lo) & (Em <= hi)
    E, y = Em[use], Im[use]

    def solve(shift):
        x = np.interp(E - shift, Ep, Ip)
        gain, offset = np.polyfit(x, y, 1)
        resid = y - (gain * x + offset)
        return gain, offset, resid

    shifts = np.arange(-MAX_SHIFT_MV, MAX_SHIFT_MV + 1e-9, SHIFT_STEP_MV) / 1e3
    rmse = np.array([np.sqrt(np.mean(solve(s)[2] ** 2)) for s in shifts])
    best = shifts[rmse.argmin()]
    gain, offset, resid = solve(best)

    x0 = np.interp(E, Ep, Ip)                         # PalmSens on my grid, nothing corrected
    span = np.ptp(x0)                                 # PalmSens signal size inside the window
    g0, b0, r0 = solve(0.0)                           # gain + offset only, no shift allowed
    return dict(E=E, y=y, x0=x0, shifts=shifts, rmse_curve=rmse, resid=resid,
                gain=gain, offset=offset, shift=best, span=span,
                at_edge=abs(best) > max_s - 2.5 * SHIFT_STEP_MV / 1e3,   # at (or right next to) the search limit
                rmse_raw=np.sqrt(np.mean((y - x0) ** 2)),
                rmse_noshift=np.sqrt(np.mean(r0 ** 2)),
                rmse_fit=np.sqrt(np.mean(resid ** 2)),
                r2=1 - np.sum(resid ** 2) / np.sum((y - y.mean()) ** 2),
                palm_sorted=(Ep, Ip))


fits = []
for my_file, ps_file in PAIRS:
    mine, palm = load_mine(my_file), load_palmsens(ps_file)
    f = aligned_fit(mine, palm)
    f.update(my_name=Path(my_file).stem, ps_name=Path(ps_file).stem, mine=mine, palm=palm)
    fits.append(f)
    print(f"OpenStat {f['my_name']} vs PalmSens {f['ps_name']}:  gain {f['gain']:.3f}   "
          f"offset {f['offset'] * SCALE:+.2f} {U}   shift {f['shift'] * 1e3:+.1f} mV   "
          f"residual {f['rmse_fit'] * SCALE:.2f} {U} ({100 * f['rmse_fit'] / f['span']:.1f} % of PalmSens span)"
          + ('   <-- shift hit the search limit' if f['at_edge'] else ''))

# ## 1. Before and after alignment
#
# Left: the two raw curves plus the PalmSens curve after applying the fitted gain, offset and shift (dotted).
# Middle: the residual, OpenStat minus aligned PalmSens. Right: fit error against trial shift — a sharp
# minimum means the shift is well determined, a flat curve means it is not.

def xdir(ax):
    if FLIP_X_AXIS:
        ax.invert_xaxis()
    ax.set_xlabel('E [V]'); ax.grid(alpha=0.3)

fig, ax = plt.subplots(len(fits), 3, figsize=(18, 4.6 * len(fits)), dpi=110, squeeze=False)
for row, f in zip(ax, fits):
    (Em, Im), (Ep, Ip) = f['mine'], f['palm_sorted']
    a = row[0]
    a.plot(Ep, Ip * SCALE, color='tab:orange', lw=1.8, label=f"PalmSens {f['ps_name']}")
    a.plot(Em, Im * SCALE, color='tab:blue', lw=2, label=f"OpenStat {f['my_name']}")
    a.plot(Ep + f['shift'], (f['gain'] * Ip + f['offset']) * SCALE, color='k', lw=1.5, ls=':',
           label='PalmSens after gain/offset/shift')
    a.axvspan(f['E'].min(), f['E'].max(), color='grey', alpha=0.08, label='fit window')
    a.set_ylabel(f'ΔI [{U}]'); a.legend(fontsize=8); xdir(a)
    a.set_title(f"{f['my_name']} vs {f['ps_name']}:  gain {f['gain']:.3f}, "
                f"offset {f['offset'] * SCALE:+.2f} {U}, shift {f['shift'] * 1e3:+.1f} mV")

    a = row[1]
    a.axhline(0, color='k', lw=0.8)
    a.plot(f['E'], f['resid'] * SCALE, color='tab:purple')
    a.fill_between(f['E'], 0, f['resid'] * SCALE, color='tab:purple', alpha=0.2)
    a.set_ylabel(f'residual [{U}]'); xdir(a)
    a.set_title(f"Residual: RMS {f['rmse_fit'] * SCALE:.2f} {U} = {100 * f['rmse_fit'] / f['span']:.1f} % of PalmSens span")

    a = row[2]
    a.plot(f['shifts'] * 1e3, f['rmse_curve'] * SCALE, color='tab:green')
    a.axvline(f['shift'] * 1e3, color='k', ls='--', lw=1)
    a.set_xlabel('trial shift [mV]'); a.set_ylabel(f'fit RMS error [{U}]'); a.grid(alpha=0.3)
    a.set_title(f"Best shift {f['shift'] * 1e3:+.1f} mV" + ('  (at search limit!)' if f['at_edge'] else ''))
plt.tight_layout()
if SAVE_AS:
    fig.savefig(SAVE_AS, dpi=200)
plt.show()

# ## 2. Residuals of all pairs together

fig, ax = plt.subplots(figsize=(12, 5), dpi=110)
ax.axhline(0, color='k', lw=0.8)
for f, c in zip(fits, plt.cm.tab10(range(len(fits)))):
    ax.plot(f['E'], 100 * f['resid'] / f['span'], color=c, lw=1.8,
            label=f"{f['my_name']} vs {f['ps_name']}")
ax.set_ylabel('residual [% of PalmSens span]'); ax.legend(); xdir(ax)
ax.set_title('What is left after gain, offset and shift are removed')
plt.tight_layout()
plt.show()

# ## 3. Numbers
#
# The three RMS rows show how much each correction buys: nothing corrected → gain and offset only →
# gain, offset and shift. *Span* is the size of the PalmSens signal (max − min) inside the fit window.

table = pd.DataFrame({
    f"{f['my_name']} vs {f['ps_name']}": {
        'gain': round(f['gain'], 3),
        f'offset [{U}]': round(f['offset'] * SCALE, 3),
        'shift [mV]': round(f['shift'] * 1e3, 1),
        f'RMS, nothing corrected [{U}]': round(f['rmse_raw'] * SCALE, 3),
        f'RMS, gain + offset [{U}]': round(f['rmse_noshift'] * SCALE, 3),
        f'RMS, gain + offset + shift [{U}]': round(f['rmse_fit'] * SCALE, 3),
        'residual [% of span]': round(100 * f['rmse_fit'] / f['span'], 2),
        f'largest residual [{U}]': round(np.abs(f['resid']).max() * SCALE, 3),
        'largest residual at E [V]': round(f['E'][np.abs(f['resid']).argmax()], 3),
        'R² after fit': round(f['r2'], 4),
        f'PalmSens span [{U}]': round(f['span'] * SCALE, 2),
        'fit window [V]': f"{f['E'].min():.3f} to {f['E'].max():.3f}",
    } for f in fits})
print(
    table
)
