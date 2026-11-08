import pandas as pd

df = pd.read_csv('data/highres_events_sheet2.csv')

for idx, (eid, g) in enumerate(df.groupby('Event ID', sort=False), 1):
    evt_name = g['Event Name'].iloc[0]
    t_min = g['Elapsed Time (s)'].min()
    t_max = g['Elapsed Time (s)'].max()
    states = g['Tap Flow State'].value_counts().to_dict()
    p_min = g['Gauge Pressure (kPa)'].min()
    p_max = g['Gauge Pressure (kPa)'].max()
    p_mean = g['Gauge Pressure (kPa)'].mean()
    flow_max = g['Flow Rate (L/s)'].max()
    vol_max = g['Cumulative Volume (L)'].max() - g['Cumulative Volume (L)'].min()
    us_min = g['Ultrasonic Ground Truth (%)'].min()
    us_max = g['Ultrasonic Ground Truth (%)'].max()
    pest_min = g['Pressure Est. Level (%)'].min()
    pest_max = g['Pressure Est. Level (%)'].max()
    
    # baseline static pressure
    static_rows = g[g['Tap Flow State'] == 'STATIC']
    p_base = static_rows['Gauge Pressure (kPa)'].iloc[0] if len(static_rows) > 0 else g['Gauge Pressure (kPa)'].iloc[0]
    
    print(f"[{idx:02d}] {eid} | {evt_name}")
    print(f"     Samples: {len(g)} | Time: {t_min:.2f}s -> {t_max:.2f}s (dt = {t_max-t_min:.2f}s)")
    print(f"     States: {states}")
    print(f"     Pressure: Baseline={p_base:.2f} kPa, Min={p_min:.2f} kPa, Max={p_max:.2f} kPa (Drop={p_base-p_min:.2f} kPa, Surge={p_max-p_base:.2f} kPa)")
    print(f"     Flow: Max={flow_max:.3f} L/s, Delta Vol={vol_max:.3f} L")
    print(f"     US Level: [{us_min:.1f}%, {us_max:.1f}%] | Pressure Level: [{pest_min:.1f}%, {pest_max:.1f}%] (Max Error: {abs(us_min-pest_min):.1f}%)")
    print()
