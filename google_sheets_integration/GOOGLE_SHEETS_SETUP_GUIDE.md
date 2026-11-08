# Google Sheets Synchronization Setup Guide

This guide explains how to set up the **Google Sheets backend** to receive synchronized readings from:
1. **ESP32 Node 1**: Tap Head Pressure Sensor Node
2. **ESP32 Node 2**: Overhead Tank Ultrasonic Sensor Node (Ground Reality)

Both devices log their data at the exact same 10-minute intervals (e.g. `12:00`, `12:10`, `12:20`, ...), and Google Apps Script merges them into the **same row**, computing the estimation error ($\Delta$) automatically!

---

## Step 1: Create a Google Spreadsheet

1. Go to [Google Sheets](https://sheets.google.com) and create a **Blank spreadsheet**.
2. Rename the spreadsheet to something like:
   `Smart_Tank_Water_Level_Logs`

---

## Step 2: Open Apps Script Editor

1. In your Google Spreadsheet, click on **Extensions** in the top menu bar.
2. Select **Apps Script**.
3. A new tab will open with the Apps Script code editor.
4. Delete any default code in `Code.gs`.
5. Copy the entire code from `google_sheets_integration/GoogleAppsScript_Code.gs` and paste it into the editor.
6. Click the **Save** icon (diskette icon) or press `Ctrl + S`.

---

## Step 3: Deploy as a Web App

1. In the top right corner of the Apps Script window, click the blue **Deploy** button and choose **New deployment**.
2. Click the gear icon (⚙️) next to "Select type" and select **Web app**.
3. Fill in the deployment configuration:
   - **Description**: `ESP32 Dual-Node Water Level Logger`
   - **Execute as**: `Me (your_email@gmail.com)`
   - **Who has access**: **`Anyone`** *(Important: Must be 'Anyone' so the ESP32 can send HTTP POST/GET requests without complex Google OAuth tokens!)*
4. Click **Deploy**.
5. Google will ask you to **Authorize access**:
   - Click *Authorize access*.
   - Choose your Google account.
   - If you see "Google hasn't verified this app", click *Advanced* (bottom left) and click *Go to Untitled project (unsafe)*.
   - Click *Allow*.
6. Copy the **Web App URL**. It will look like:
   ```text
   https://script.google.com/macros/s/AKfycbxXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX/exec
   ```
7. Keep this URL handy. You will paste this URL into `config.h` of both ESP32 programs.

---

## Step 4: Verification / Test Call

You can test the deployment immediately from your browser or PowerShell:

### Test Pressure Node (Node 1):
Open this URL in your web browser (replace with your actual script URL):
```text
https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec?node=pressure&voltage=1.85&pressure_kpa=45.2&level_pct=72.5&head_m=4.61&tap_state=STATIC
```

### Test Ultrasonic Node (Node 2):
Open this URL in another browser tab:
```text
https://script.google.com/macros/s/YOUR_SCRIPT_ID/exec?node=ultrasonic&distance_cm=41.2&level_pct=73.8
```

Go back to your Google Sheet:
You will see a tab called **`WaterLevel_Synchronized_Logs`** created automatically!
Both readings will be aligned in the **same row** under the matching 1-minute timestamp bucket, and the **Absolute Error Δ (%)** and **Error in Depth (cm)** will be calculated in columns I and J automatically!

---

## Step 5: Plotting Comparison Graph (Rows 2283 to 2654) & 6.0 cm Full Calibration

When evaluating model predictions against ultrasound ground reality for your dissertation (rows 2283 to 2654):

### Physical Baseline:
- **Tank Full Distance**: **`6.0 cm`** (air gap measured from the ultrasound sensor face down to the water surface when the tank is at maximum 100% full capacity).
- **Tank Depth**: **`122.0 cm`** (usable water column height).
- **Physical Water Height**:
  $$\text{Height (cm)} = 122.0 - (\text{Distance}_{\text{measured}} - 6.0)$$
- **Ground Truth Water Level (%)**:
  $$\text{Level (\%)} = \left(\frac{122.0 - (\text{Distance}_{\text{measured}} - 6.0)}{122.0}\right) \times 100\% = \left(1 - \frac{\text{Distance}_{\text{measured}} - 6.0}{122.0}\right) \times 100\%$$

---

### Method 1: One-Click via Google Sheets Custom Menu (Recommended)
1. Open your Google Spreadsheet in Google Chrome.
2. In the top Google Sheets menu bar, click the custom menu:
   **`📊 Water Level Analysis`**
3. Click:
   - **`🚀 Run Both: Recalibrate & Plot Graph (Rows 2283 - 2654)`** (or individually: **`🎯 Recalculate Ground Truth (6.0 cm = Full Tank)`** then **`📈 Plot Comparison Graph (Rows 2283 - 2654)`**).
4. Google Sheets will automatically:
   - Recalculate all ultrasonic ground truth % values in **Column H** using 6.0 cm as full tank.
   - Update Absolute Error $\Delta$ in **Column I** and Depth Error in **Column J**.
   - Embed a line comparison chart at **Column P** on `WaterLevel_Synchronized_Logs`.
   - Create a dedicated analysis tab: **`Comparison_2283_2654`** containing:
     - MAE (Mean Absolute Error)
     - RMSE (Root Mean Square Error)
     - Max Error
     - Clean line comparison graph with complete legend and axis labels!

---

### Method 2: One-Click via Cloud Web Dashboard
1. Open your deployed Web App URL in your browser.
2. Scroll to the **Dissertation Evaluation (Rows 2283 - 2654)** card.
3. Verify Start Row = `2283`, End Row = `2654`, and Full Dist = `6.0`.
4. Click **🚀 Calibrate & Plot Graph**.

---

### Method 3: Direct Google Sheets Cell Formulas
If you want to apply or inspect the exact formula in Google Sheets cells:
1. **Ultrasonic Ground Truth (%)** (Column H, Row 2283):
   ```excel
   =IF(ISBLANK(G2283), "", MAX(0, MIN(100, ROUND((1 - (G2283 - 6.0) / 122) * 100, 1))))
   ```
   *Drag down from H2283 to H2654.*
2. **Absolute Error Δ (%)** (Column I, Row 2283):
   ```excel
   =IF(OR(ISBLANK(D2283), ISBLANK(H2283)), "", ROUND(ABS(D2283 - H2283), 2))
   ```
   *Drag down from I2283 to I2654.*
3. **Depth Error (cm)** (Column J, Row 2283):
   ```excel
   =IF(ISBLANK(I2283), "", ROUND((I2283 / 100) * 122, 2))
   ```
   *Drag down from J2283 to J2654.*

---

### Method 4: Manual Chart Insertion via Google Sheets UI
1. In `WaterLevel_Synchronized_Logs`, select cell range `A2283:A2654`.
2. Hold `Ctrl` (or `Cmd` on Mac) and select cell range `D2283:D2654`.
3. Keep holding `Ctrl` and select cell range `H2283:H2654`.
4. Click **Insert** > **Chart** in the top menu.
5. In the **Chart Editor** sidebar (right side):
   - **Chart type**: Select **Line chart** (or Smooth line chart).
   - **X-axis**: Confirm `A2283:A2654` (Timestamp Slot).
   - **Series**: Confirm `D2283:D2654` (Pressure Predicted Level %) and `H2283:H2654` (Ultrasonic Ground Truth %).
6. Under **Customize**:
   - **Chart & axis titles**: Set Title to `"Water Level Prediction vs Ultrasonic Ground Truth (Rows 2283 - 2654)"`.
   - **Horizontal axis**: Set title to `"Time Slot (1-Min Intervals)"`.
   - **Vertical axis**: Set title to `"Water Level (%)"`, Min: `0`, Max: `100`.
   - **Series Colors**: Blue for Prediction (`#0284c7`), Green for Ground Truth (`#10b981`).

---

### Method 5: Standalone 300 DPI Python Figure Generation
To generate publication-quality figures for your thesis report:
```powershell
python a:\Dissertation\code\plot_water_level_comparison.py --start 2283 --end 2654
```
*Outputs: `water_level_comparison_rows_2283_2654.png` with MAE, RMSE, Pearson correlation, and residual error bands.*

---

## Step 6: 24-Hour Resilient Architecture (Offline Storage & Auto-Reconnection)

### What Happened at 1:27 AM (Root Cause)?
* **DHCP Lease Renewal / AP Glitch**: Most home routers (e.g. JioFiber) periodically refresh DHCP IP leases or perform 6-to-12-hour Wi-Fi beacon renegotiation. At 1:27 AM, the router momentarily dropped or re-keyed the wireless connection.
* **Missing Reconnect Loop**: The original firmware only called `initWiFi()` in `setup()`. Once the connection was lost, `WiFi.status()` changed to `WL_DISCONNECTED`. Without background auto-reconnection, the ESP32 stayed stranded offline until a physical power restart.
* **Blocking NTP Call**: Calling `getLocalTime()` when Wi-Fi is lost caused time synchronization attempts to fail.

### The New Bulletproof Defense System:
1. **Flash SPIFFS Offline Queue**:
   * If Wi-Fi drops or Google Sheets returns an HTTP error, the ESP32 appends each 1-minute record to `/offline_pressure.csv` or `/offline_ultrasonic.csv` in persistent Flash memory.
   * Power-fail safe: Even if powered off and on, offline records are safely preserved in Flash.
2. **Resilient Hardware RTC Clock**:
   * Uses internal quartz RTC (`time(nullptr)` and `localtime_r`).
   * The clock continues ticking with 1-second accuracy even during hours of Wi-Fi absence.
3. **Background Auto-Reconnection (`maintainWiFi()`)**:
   * Constantly monitors connection health every 5 seconds.
   * Automatically calls `WiFi.reconnect()`. If disconnected for > 3 minutes, it resets the radio hardware (`WiFi.disconnect(true); WiFi.begin(...)`).
4. **Bulk Backlog Ingestion (Google Apps Script)**:
   * As soon as Wi-Fi returns, the ESP32 flushes its offline backlog in efficient batches of 25 records.
   * `GoogleAppsScript_Code.gs` ingests the entire historical backlog and aligns every record with its exact historical timestamp in Google Sheets!
