import pandas as pd
import numpy as np

df = pd.read_csv('data/highres_events_sheet2.csv')

print("Columns in Sheet 2:", df.columns.tolist())
print(f"Total rows: {len(df)}")

# Group by Event ID and Event Name
grouped = df.groupby(['Event ID', 'Event Name'], sort=False)

event_summary = []
for i, ((evt_id, evt_name), group) in enumerate(grouped):
    p = group['Gauge Pressure (kPa)'].dropna()
    dp = group['dP/dt (kPa/s)'].dropna()
    u_dist = group['Ultrasonic Distance (cm)'].dropna()
    u_lvl = group['Ultrasonic Ground Truth (%)'].dropna()
    p_lvl = group['Pressure Est. Level (%)'].dropna()
    t = group['Elapsed Time (s)'].dropna()
    states = group['Tap Flow State'].unique().tolist()
    
    t_start = group['Timestamp (High-Res)'].iloc[0]
    t_end = group['Timestamp (High-Res)'].iloc[-1]
    
    event_summary.append({
        'Index': i + 1,
        'Event ID': evt_id,
        'Event Name': evt_name,
        'Samples': len(group),
        'Duration (s)': t.max() - t.min() if len(t) > 0 else 0,
        'Start Time': t_start,
        'End Time': t_end,
        'States': ", ".join(states),
        'P_min': p.min(),
        'P_max': p.max(),
        'P_mean': p.mean(),
        'dP/dt_min': dp.min(),
        'dP/dt_max': dp.max(),
        'US_Dist_mean': u_dist.mean(),
        'US_Lvl_mean': u_lvl.mean(),
        'P_Lvl_mean': p_lvl.mean(),
    })

summary_df = pd.DataFrame(event_summary)
pd.set_option('display.max_columns', None)
pd.set_option('display.width', 1000)
print("\n" + "="*80)
print("HIGH-RES EVENTS SUMMARY (SHEET 2)")
print("="*80)
print(summary_df[['Index', 'Event ID', 'Event Name', 'Samples', 'Duration (s)', 'States', 'P_min', 'P_max', 'dP/dt_min', 'dP/dt_max']])
print("\n" + "="*80)
print("LEVEL & ACOUSTIC GROUND TRUTH SUMMARY")
print("="*80)
print(summary_df[['Index', 'Event Name', 'US_Dist_mean', 'US_Lvl_mean', 'P_Lvl_mean', 'Start Time', 'End Time']])
