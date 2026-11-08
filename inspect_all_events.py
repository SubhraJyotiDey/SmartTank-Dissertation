import pandas as pd
import numpy as np

df = pd.read_csv('data/highres_events_sheet2.csv')

print(f"Total rows: {len(df)}")
unique_events = df[['Event ID', 'Event Name']].drop_duplicates()
print(f"Unique Events Count: {len(unique_events)}")

results = []
for idx, (eid, ename) in enumerate(unique_events.values, 1):
    g = df[df['Event ID'] == eid]
    
    p = g['Gauge Pressure (kPa)']
    dp = g['dP/dt (kPa/s)']
    states = g['Tap Flow State'].tolist()
    u_lvl = g['Ultrasonic Ground Truth (%)']
    p_lvl = g['Pressure Est. Level (%)']
    flow = g['Flow Rate (L/s)']
    vol = g['Cumulative Volume (L)']
    elapsed = g['Elapsed Time (s)']
    
    # Static baseline (first few rows or STATIC rows)
    static_rows = g[g['Tap Flow State'] == 'STATIC']
    p_static_init = static_rows['Gauge Pressure (kPa)'].iloc[0] if len(static_rows) > 0 else p.iloc[0]
    
    # Minimum pressure during event (dynamic friction trough)
    p_min = p.min()
    p_min_t = elapsed.iloc[p.argmin()]
    delta_p_drop = p_static_init - p_min
    
    # Maximum pressure during event (water hammer surge)
    p_max = p.max()
    p_max_t = elapsed.iloc[p.argmax()]
    delta_p_surge = p_max - p_static_init
    
    # State duration
    open_count = states.count('TAP_OPEN')
    settling_count = states.count('SETTLING')
    static_count = states.count('STATIC')
    
    # Uncompensated naive pressure level:
    # If Level % = (P - pEmpty) / (pFull - pEmpty) * 100
    # From the sheet, let's see p_lvl min and max
    p_lvl_min = p_lvl.min()
    p_lvl_max = p_lvl.max()
    u_lvl_mean = u_lvl.mean()
    
    # Max error of naive pressure estimate vs ultrasonic ground truth
    naive_error = np.max(np.abs(p_lvl - u_lvl))
    
    # Total volume discharged
    vol_discharged = vol.max() - vol.min()
    
    results.append({
        'Index': idx,
        'Event ID': eid,
        'Event Name': ename,
        'Duration (s)': elapsed.max() - elapsed.min(),
        'Samples': len(g),
        'P_baseline (kPa)': round(p_static_init, 2),
        'P_min (kPa)': round(p_min, 2),
        'P_min_t (s)': round(p_min_t, 2),
        'Delta_P_drop (kPa)': round(delta_p_drop, 2),
        'P_hammer_peak (kPa)': round(p_max, 2),
        'P_peak_t (s)': round(p_max_t, 2),
        'Delta_P_hammer (kPa)': round(delta_p_surge, 2),
        'Max_dPdt (kPa/s)': round(dp.max(), 1),
        'Min_dPdt (kPa/s)': round(dp.min(), 1),
        'US_Truth_mean (%)': round(u_lvl_mean, 2),
        'P_Est_min (%)': round(p_lvl_min, 1),
        'P_Est_max (%)': round(p_lvl_max, 1),
        'Max_Error (%)': round(naive_error, 1),
        'Vol_Discharged (L)': round(vol_discharged, 3),
        'Open_Samples': open_count,
        'Settling_Samples': settling_count,
        'Static_Samples': static_count
    })

res_df = pd.DataFrame(results)
pd.set_option('display.max_columns', None)
pd.set_option('display.width', 1000)
print(res_df.to_string(index=False))

res_df.to_csv('plots/highres_events/highres_events_dissertation_summary.csv', index=False)
print("\nSaved summary CSV to plots/highres_events/highres_events_dissertation_summary.csv")
