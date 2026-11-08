import urllib.request
import os
import pandas as pd

os.makedirs('data', exist_ok=True)
csv_path = 'data/highres_events_sheet2.csv'

spreadsheet_id = '1g9Ocbs7dvUK6dxU7HSsXEuSEoDcVO5r1l1RQtHeOrno'
url = f'https://docs.google.com/spreadsheets/d/{spreadsheet_id}/gviz/tq?tqx=out:csv&sheet=HighRes_Event_Logs'

print(f"Fetching from: {url}")
req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
with urllib.request.urlopen(req) as resp:
    data = resp.read().decode('utf-8')
    with open(csv_path, 'w', encoding='utf-8') as f:
        f.write(data)

print(f"Saved {len(data)} bytes to {csv_path}")

df = pd.read_csv(csv_path)
print(f"Loaded DataFrame with {len(df)} rows and {len(df.columns)} columns:")
print("Columns:", list(df.columns))

# Group by Event ID and Event Name
events = df.groupby(['Event ID', 'Event Name'], sort=False).size().reset_index(name='Sample_Count')
print(f"\nTotal distinct (Event ID, Event Name) pairs: {len(events)}")
for idx, row in events.iterrows():
    print(f"Event {idx+1:02d}: [{row['Event ID']}] '{row['Event Name']}' -> {row['Sample_Count']} samples")
