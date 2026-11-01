# DPV comparison analysis

Python scripts comparing OpenStat DPV sweeps against PalmSens reference measurements.
The measurement data is not stored in this repository. Pass the folder that contains `Palmsens/`
(`.pssession`) and `My Device/` (`.csv`):

```
pip install -r requirements.txt
python best_fc_comparison.py "path/to/Sunday Oct 11"
```

| Script | Purpose |
|---|---|
| `simple_plot.py` | Overlay selected PalmSens and OpenStat runs |
| `palmsens_vs_openstat.py` | Early salt comparison, including `sample_90` |
| `best_fc_comparison.py` | Correlate all FC runs and plot the best three pairs |
| `best_salt_comparison.py` | Compare the chosen salt runs and correlate all salt runs |
| `aligned_fit_fc.py` | Fit gain, offset and potential shift for the FC pairs |
| `aligned_fit_salt.py` | Fit gain, offset and potential shift for the salt pairs |
| `select_best_salt.py` | Search for the most correlated 3-by-3 salt set |
| `compare_salt_2x_5x.py` | Compare OpenStat `Salt_2_x` and `Salt_5_x` with PalmSens runs |

Settings are near the top of each script. Figures open in windows; set `MPLBACKEND=Agg` to run
without them.
