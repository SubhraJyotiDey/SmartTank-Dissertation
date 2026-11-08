import pandas as pd
df = pd.read_csv('data/highres_events_sheet2.csv')
g = df[df['Event ID'] == 'EVT_1728']
print(g[['Elapsed Time (s)', 'Gauge Pressure (kPa)', 'dP/dt (kPa/s)', 'Tap Flow State']].to_string())
