import pandas as pd
import numpy as np

df = pd.read_csv('data/highres_events_sheet2.csv')

print("="*80)
print("EVALUATION OF TAP_OPEN STATE ACCURACY ACROSS ALL 13 EVENTS")
print("="*80)

for idx, (eid, g) in enumerate(df.groupby('Event ID', sort=False), 1):
    ename = g['Event Name'].iloc[0]
    p = g['Gauge Pressure (kPa)'].values
    t = g['Elapsed Time (s)'].values
    st = g['Tap Flow State'].values
    dp = g['dP/dt (kPa/s)'].values
    
    # Static baseline P0 (first 10 samples or initial quiescent state)
    p0 = np.median(p[:10])
    
    # Physical Flow Detection:
    # During physical flow, Bernoulli/Darcy dynamic loss causes P to drop significantly below P0
    # In this rig, a single tap open causes ~12-14 kPa drop; dual tap causes ~32-40 kPa drop.
    # A conservative threshold for active physical discharge is (P0 - P) > 4.0 kPa.
    is_physically_discharging = (p0 - p) > 4.0
    
    # ESP32 classifications:
    esp_open = (st == 'TAP_OPEN')
    esp_settling = (st == 'SETTLING')
    esp_static = (st == 'STATIC')
    
    # Overlap / Metrics:
    true_flow_samples = np.sum(is_physically_discharging)
    esp_open_samples = np.sum(esp_open)
    
    # True positive: physically discharging and marked TAP_OPEN
    tp = np.sum(is_physically_discharging & esp_open)
    # False negative: physically discharging but marked SETTLING or STATIC
    fn_settling = np.sum(is_physically_discharging & esp_settling)
    fn_static = np.sum(is_physically_discharging & esp_static)
    # False positive: quiescent or surging but marked TAP_OPEN
    fp = np.sum((~is_physically_discharging) & esp_open)
    
    print(f"[{idx:02d}] {eid} | {ename} (Total Samples: {len(g)}, Dur: {t[-1]-t[0]:.1f}s)")
    print(f"     P0 = {p0:.2f} kPa | P_min = {np.min(p):.2f} kPa (Loss = {p0 - np.min(p):.2f} kPa)")
    print(f"     Physical Discharge Samples (Loss > 4 kPa): {true_flow_samples} ({true_flow_samples*0.1:.1f}s)")
    print(f"     ESP32 Classified TAP_OPEN Samples       : {esp_open_samples} ({esp_open_samples*0.1:.1f}s)")
    print(f"     ESP32 Correctly Captured (TP)           : {tp} samples ({tp*0.1:.1f}s)")
    print(f"     Prematurely marked SETTLING during flow : {fn_settling} samples ({fn_settling*0.1:.1f}s)")
    print(f"     Prematurely marked STATIC during flow   : {fn_static} samples ({fn_static*0.1:.1f}s)")
    print(f"     Quiescent samples falsely marked OPEN   : {fp} samples ({fp*0.1:.1f}s)")
    print()
