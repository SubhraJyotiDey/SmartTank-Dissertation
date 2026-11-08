import pandas as pd
import numpy as np

df = pd.read_csv('data/highres_events_sheet2.csv')

for eid, g in df.groupby('Event ID', sort=False):
    ename = g['Event Name'].iloc[0]
    p = g['Gauge Pressure (kPa)'].values
    t = g['Elapsed Time (s)'].values
    st = g['Tap Flow State'].values
    dp = g['dP/dt (kPa/s)'].values
    
    # Check where pressure drops significantly (> 5 kPa below baseline)
    p0 = p[0]
    is_pressure_dropped = (p0 - p) > 5.0
    
    # Compare with st == 'TAP_OPEN'
    open_mask = (st == 'TAP_OPEN')
    settling_mask = (st == 'SETTLING')
    static_mask = (st == 'STATIC')
    
    # Samples where pressure is dropped but state is STATIC:
    dropped_but_static = np.sum(is_pressure_dropped & static_mask)
    # Samples where pressure is dropped but state is SETTLING:
    dropped_but_settling = np.sum(is_pressure_dropped & settling_mask)
    # Samples where state is TAP_OPEN:
    total_open = np.sum(open_mask)
    total_dropped = np.sum(is_pressure_dropped)
    
    print(f"=== {eid}: {ename} ===")
    print(f"  P0: {p0:.2f} kPa, P_min: {np.min(p):.2f} kPa")
    print(f"  Total samples: {len(g)}")
    print(f"  Samples with (P0 - P > 5 kPa): {total_dropped}")
    print(f"  Samples marked TAP_OPEN: {total_open}")
    print(f"  Samples marked SETTLING: {np.sum(settling_mask)}")
    print(f"  Samples marked STATIC: {np.sum(static_mask)}")
    print(f"  Samples with dropped pressure marked SETTLING: {dropped_but_settling}")
    print(f"  Samples with dropped pressure marked STATIC: {dropped_but_static}")
    
    # State transitions:
    transitions = []
    for i in range(1, len(st)):
        if st[i] != st[i-1]:
            transitions.append(f"{st[i-1]} -> {st[i]} at t={t[i]:.2f}s (P={p[i]:.1f} kPa, dP/dt={dp[i]:.1f})")
    print("  Transitions:")
    for tr in transitions:
        print("    ", tr)
    print()
