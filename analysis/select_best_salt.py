"""Search all salt runs for the most correlated 3-by-3 set of OpenStat and PalmSens runs.

Usage: python select_best_salt.py [data folder containing "Palmsens" and "My Device"]
"""
import glob
import json
import os
import sys
from itertools import combinations
from pathlib import Path

import numpy as np
import pandas as pd

pd.set_option('display.width', 250); pd.set_option('display.max_columns', 30)
os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')


def load_palmsens(path):
    m = json.loads(Path(path).read_bytes().decode('utf-16').strip('﻿\x00 \r\n'))['Measurements'][0]
    a = {x['Description']: np.array([q['V'] for q in x['DataValues']]) for x in m['DataSet']['Values']}
    return a['potential'], a['current'] * 1e3


def dedupe(runs):
    kept = {}
    for name, (E, I) in runs.items():
        twin = next((k for k, (_, I2) in kept.items() if len(I2) == len(I) and np.array_equal(I, I2)), None)
        if twin:
            print('duplicate', name, '==', twin)
        else:
            kept[name] = (E, I)
    return kept


P = dedupe({Path(p).stem: load_palmsens(p) for p in sorted(glob.glob('Palmsens/Salt*.pssession'))})
D = {}
for p in sorted(glob.glob('My Device/Salt*.csv')) + sorted(glob.glob('My Device/sample*.csv')):
    d = pd.read_csv(p)
    if d.E_applied_V.iloc[0] < d.E_applied_V.iloc[-1]:
        print('skipping reverse scan', p)
        continue
    D[Path(p).stem] = (d.E_applied_V.values, -d.dI_nA.values)
D = dedupe(D)

G = np.arange(-0.194, 0.6001, 0.005)


def on_grid(run):
    order = np.argsort(run[0])
    return np.interp(G, run[0][order], run[1][order])


for name, sel in [('FULL', G < 9), ('E < 0.45 V', G < 0.45)]:
    C = pd.DataFrame({p: {m: np.corrcoef(on_grid(D[m])[sel], on_grid(P[p])[sel])[0, 1] for m in D} for p in P})
    print('\n=====', name)
    print(C.round(3))
    blocks = []
    for ms in combinations(C.index, 3):
        for ps in combinations(C.columns, 3):
            b = C.loc[list(ms), list(ps)].values
            blocks.append((b.mean(), b.min(), ms, ps))
    blocks.sort(reverse=True)
    print('top blocks by mean r:')
    for b in blocks[:5]:
        print('  mean %.4f min %.4f' % b[:2], b[2], b[3])
