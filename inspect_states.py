import pandas as pd

df = pd.read_csv('data/highres_events_sheet2.csv')

for eid, g in df.groupby('Event ID', sort=False):
    print(f"=== {eid} ===")
    for state in ['STATIC', 'TAP_OPEN', 'SETTLING']:
        sub = g[g['Tap Flow State'] == state]
        if len(sub) > 0:
            print(f"  {state:10s}: P=[{sub['Gauge Pressure (kPa)'].min():.1f}, {sub['Gauge Pressure (kPa)'].max():.1f}] kPa | P_Est_Lvl=[{sub['Pressure Est. Level (%)'].min():.1f}, {sub['Pressure Est. Level (%)'].max():.1f}] % | US_Lvl=[{sub['Ultrasonic Ground Truth (%)'].min():.1f}, {sub['Ultrasonic Ground Truth (%)'].max():.1f}] %")
