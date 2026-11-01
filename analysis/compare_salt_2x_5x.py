"""Compare OpenStat Salt_2_x and Salt_5_x runs against selected PalmSens salt runs.

Usage: python compare_salt_2x_5x.py [data folder containing "Palmsens" and "My Device"]
"""
import json
import os
import sys
from pathlib import Path

import numpy as np
import pandas as pd

os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')


def load_palmsens(path):
    m = json.loads(Path(path).read_bytes().decode('utf-16').strip('﻿\x00 \r\n'))['Measurements'][0]
    a = {x['Description']: np.array([q['V'] for q in x['DataValues']]) for x in m['DataSet']['Values']}
    return a['potential'], a['current'] * 1e3


G = np.arange(-0.194, 0.6001, 0.005)


def on_grid(E, I):
    order = np.argsort(E)
    return np.interp(G, E[order], I[order])


P = {k: on_grid(*load_palmsens(f'Palmsens/{k}.pssession')) for k in ['Salt_2_1', 'Salt_2_2', 'Salt_4_1', 'Salt_5_1']}
D = {}
for k in ['Salt_2_1', 'Salt_2_2', 'Salt_5_1', 'Salt_5_2', 'Salt_5_3']:
    d = pd.read_csv(f'My Device/{k}.csv')
    D[k] = on_grid(d.E_applied_V.values, -d.dI_nA.values)
    print(k, 'first %.0f' % -d.dI_nA.iloc[0], 'rtia', d.rtia_code.unique())

low = G < 0.45
for m in D:
    for p in P:
        e = D[m] - P[p]
        print(f'OS {m} vs PS {p}: r_full {np.corrcoef(D[m], P[p])[0, 1]:.3f}  '
              f'r_low {np.corrcoef(D[m][low], P[p][low])[0, 1]:.3f}  '
              f'rmse_full {np.sqrt((e ** 2).mean()):.1f}  rmse_low {np.sqrt((e[low] ** 2).mean()):.1f}  '
              f'offset_low {e[low].mean():+.1f}')
