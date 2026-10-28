#ifndef WEB_DASHBOARD_H
#define WEB_DASHBOARD_H

#include <Arduino.h>

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Smart Tank | Non-Invasive Hydrostatic Level Monitor</title>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
  <style>
    :root {
      --bg-dark: #090d16;
      --card-bg: rgba(18, 26, 43, 0.75);
      --card-border: rgba(255, 255, 255, 0.08);
      --card-hover: rgba(255, 255, 255, 0.12);
      --primary: #0ea5e9;
      --primary-glow: rgba(14, 165, 233, 0.35);
      --accent-cyan: #06b6d4;
      --success: #10b981;
      --warning: #f59e0b;
      --danger: #ef4444;
      --text-main: #f8fafc;
      --text-muted: #94a3b8;
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
      font-family: 'Outfit', sans-serif;
    }

    body {
      background-color: var(--bg-dark);
      background-image: 
        radial-gradient(circle at 10% 20%, rgba(14, 165, 233, 0.08) 0%, transparent 40%),
        radial-gradient(circle at 90% 80%, rgba(6, 182, 212, 0.06) 0%, transparent 40%);
      color: var(--text-main);
      min-height: 100vh;
      padding: 1.5rem;
    }

    .container {
      max-width: 1280px;
      margin: 0 auto;
    }

    /* Header */
    header {
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      align-items: center;
      gap: 1rem;
      margin-bottom: 2rem;
      padding-bottom: 1rem;
      border-bottom: 1px solid var(--card-border);
    }

    .brand-title {
      display: flex;
      align-items: center;
      gap: 0.75rem;
    }

    .brand-icon {
      width: 44px;
      height: 44px;
      background: linear-gradient(135deg, #0284c7, #06b6d4);
      border-radius: 12px;
      display: flex;
      align-items: center;
      justify-content: center;
      box-shadow: 0 0 20px var(--primary-glow);
    }

    .brand-icon svg {
      width: 24px;
      height: 24px;
      fill: #ffffff;
    }

    h1 {
      font-size: 1.5rem;
      font-weight: 700;
      letter-spacing: -0.02em;
    }

    .subtitle {
      font-size: 0.85rem;
      color: var(--text-muted);
    }

    .top-badges {
      display: flex;
      gap: 0.75rem;
      align-items: center;
      flex-wrap: wrap;
    }

    .badge {
      display: inline-flex;
      align-items: center;
      gap: 0.5rem;
      padding: 0.4rem 0.85rem;
      border-radius: 9999px;
      font-size: 0.8rem;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      backdrop-filter: blur(8px);
      border: 1px solid transparent;
    }

    .badge-static {
      background: rgba(16, 185, 129, 0.12);
      border-color: rgba(16, 185, 129, 0.3);
      color: #34d399;
    }

    .badge-flowing {
      background: rgba(245, 158, 11, 0.15);
      border-color: rgba(245, 158, 11, 0.4);
      color: #fbbf24;
      animation: pulseAlert 1.5s infinite;
    }

    .badge-settling {
      background: rgba(56, 189, 248, 0.15);
      border-color: rgba(56, 189, 248, 0.4);
      color: #38bdf8;
    }

    @keyframes pulseAlert {
      0%, 100% { opacity: 1; transform: scale(1); }
      50% { opacity: 0.8; transform: scale(0.98); }
    }

    .dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: currentColor;
    }

    /* Grid Layout */
    .grid {
      display: grid;
      grid-template-columns: 340px 1fr;
      gap: 1.5rem;
      margin-bottom: 1.5rem;
    }

    @media (max-width: 960px) {
      .grid {
        grid-template-columns: 1fr;
      }
    }

    .card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 20px;
      padding: 1.5rem;
      backdrop-filter: blur(16px);
      box-shadow: 0 10px 30px rgba(0, 0, 0, 0.4);
      transition: border-color 0.2s ease;
    }

    .card:hover {
      border-color: var(--card-hover);
    }

    .card-title {
      font-size: 1rem;
      font-weight: 600;
      color: var(--text-muted);
      text-transform: uppercase;
      letter-spacing: 0.05em;
      margin-bottom: 1.25rem;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }

    /* Tank Visualization */
    .tank-container {
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      position: relative;
    }

    .tank-glass {
      width: 170px;
      height: 260px;
      border: 3px solid rgba(255, 255, 255, 0.2);
      border-radius: 24px;
      position: relative;
      overflow: hidden;
      background: rgba(15, 23, 42, 0.6);
      box-shadow: inset 0 0 20px rgba(0, 0, 0, 0.5), 0 0 30px rgba(14, 165, 233, 0.15);
      margin-bottom: 1rem;
    }

    .tank-graduations {
      position: absolute;
      right: 8px;
      top: 15px;
      bottom: 15px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      color: rgba(255, 255, 255, 0.35);
      font-size: 0.65rem;
      font-family: 'JetBrains Mono', monospace;
      z-index: 5;
      pointer-events: none;
    }

    .water-fill {
      position: absolute;
      bottom: 0;
      left: 0;
      width: 100%;
      height: 50%;
      background: linear-gradient(180deg, rgba(56, 189, 248, 0.85) 0%, rgba(2, 132, 199, 0.95) 100%);
      transition: height 0.8s cubic-bezier(0.4, 0, 0.2, 1);
      box-shadow: 0 0 20px rgba(56, 189, 248, 0.4);
    }

    .water-wave {
      position: absolute;
      top: -12px;
      left: 0;
      width: 200%;
      height: 24px;
      background-image: url('data:image/svg+xml;utf8,<svg viewBox="0 0 800 50" xmlns="http://www.w3.org/2000/svg"><path d="M0,25 C150,50 350,0 500,25 C650,50 850,0 1000,25 L1000,50 L0,50 Z" fill="%2338bdf8" fill-opacity="0.7"/></svg>');
      background-size: 50% 100%;
      animation: waveFlow 4s linear infinite;
    }

    @keyframes waveFlow {
      0% { transform: translateX(0); }
      100% { transform: translateX(-50%); }
    }

    .tank-value-display {
      text-align: center;
      margin-top: 0.5rem;
    }

    .big-pct {
      font-size: 3rem;
      font-weight: 700;
      letter-spacing: -0.03em;
      background: linear-gradient(135deg, #ffffff, #7dd3fc);
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
    }

    .liters-label {
      font-size: 1.1rem;
      color: var(--accent-cyan);
      font-weight: 500;
    }

    /* Metric Cards Grid */
    .metrics-row {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 1rem;
      margin-bottom: 1.5rem;
    }

    .metric-card {
      background: rgba(255, 255, 255, 0.03);
      border: 1px solid var(--card-border);
      border-radius: 14px;
      padding: 1.1rem;
      position: relative;
    }

    .metric-label {
      font-size: 0.75rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      color: var(--text-muted);
      margin-bottom: 0.35rem;
    }

    .metric-val {
      font-size: 1.6rem;
      font-weight: 700;
      font-family: 'JetBrains Mono', monospace;
      color: #ffffff;
    }

    .metric-unit {
      font-size: 0.85rem;
      font-weight: 400;
      color: var(--text-muted);
      margin-left: 0.2rem;
    }

    /* Compensation Banner */
    .comp-banner {
      background: rgba(245, 158, 11, 0.08);
      border: 1px solid rgba(245, 158, 11, 0.25);
      border-radius: 12px;
      padding: 0.9rem 1.2rem;
      display: flex;
      align-items: center;
      gap: 0.85rem;
      margin-bottom: 1.5rem;
      font-size: 0.9rem;
      color: #fde68a;
    }

    /* Chart Container */
    .chart-container {
      position: relative;
      height: 230px;
      width: 100%;
    }

    /* Controls & Calibration Grid */
    .settings-grid {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 1.5rem;
    }

    @media (max-width: 800px) {
      .settings-grid {
        grid-template-columns: 1fr;
      }
    }

    .btn-group {
      display: flex;
      gap: 0.75rem;
      flex-wrap: wrap;
      margin-top: 1rem;
    }

    .btn {
      padding: 0.7rem 1.2rem;
      border-radius: 10px;
      border: none;
      font-weight: 600;
      font-size: 0.85rem;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      gap: 0.5rem;
      transition: all 0.2s ease;
    }

    .btn-primary {
      background: linear-gradient(135deg, #0284c7, #0ea5e9);
      color: white;
      box-shadow: 0 4px 15px var(--primary-glow);
    }

    .btn-primary:hover {
      transform: translateY(-2px);
      box-shadow: 0 6px 20px var(--primary-glow);
    }

    .btn-secondary {
      background: rgba(255, 255, 255, 0.08);
      color: var(--text-main);
      border: 1px solid var(--card-border);
    }

    .btn-secondary:hover {
      background: rgba(255, 255, 255, 0.15);
    }

    .btn-success {
      background: linear-gradient(135deg, #059669, #10b981);
      color: white;
    }

    .btn-warning {
      background: linear-gradient(135deg, #d97706, #f59e0b);
      color: white;
    }

    .btn-danger {
      background: linear-gradient(135deg, #b91c1c, #ef4444);
      color: white;
    }

    .btn-purple {
      background: linear-gradient(135deg, #7e22ce, #a855f7);
      color: white;
    }

    .form-group {
      margin-bottom: 1rem;
    }

    label {
      display: block;
      font-size: 0.8rem;
      color: var(--text-muted);
      margin-bottom: 0.35rem;
    }

    input[type="number"], input[type="text"] {
      width: 100%;
      background: rgba(0, 0, 0, 0.3);
      border: 1px solid var(--card-border);
      border-radius: 8px;
      padding: 0.6rem 0.8rem;
      color: #ffffff;
      font-family: 'JetBrains Mono', monospace;
      font-size: 0.9rem;
    }

    input:focus {
      outline: none;
      border-color: var(--primary);
      box-shadow: 0 0 10px var(--primary-glow);
    }

    .note-text {
      font-size: 0.8rem;
      color: var(--text-muted);
      line-height: 1.4;
      margin-top: 0.5rem;
    }

    .toast {
      position: fixed;
      bottom: 20px;
      right: 20px;
      background: #1e293b;
      border: 1px solid var(--primary);
      color: white;
      padding: 0.9rem 1.4rem;
      border-radius: 12px;
      box-shadow: 0 10px 25px rgba(0,0,0,0.5);
      transform: translateY(100px);
      opacity: 0;
      transition: all 0.3s ease;
      z-index: 100;
    }

    .toast.show {
      transform: translateY(0);
      opacity: 1;
    }
  </style>
</head>
<body>
  <div class="container">
    <!-- Top Header -->
    <header>
      <div class="brand-title">
        <div class="brand-icon">
          <svg viewBox="0 0 24 24"><path d="M12 2c-5.33 4.55-8 8.48-8 11.8 0 4.98 3.8 8.2 8 8.2s8-3.22 8-8.2c0-3.32-2.67-7.25-8-11.8z"/></svg>
        </div>
        <div>
          <h1>Smart Tank Hydrostatic Estimator</h1>
          <div class="subtitle">Non-Invasive Inline Pressure Sensing Node (ESP32)</div>
        </div>
      </div>
      <div class="top-badges">
        <div id="adcBadge" class="badge" style="background: rgba(14,165,233,0.15); border-color: rgba(14,165,233,0.3); color: #38bdf8; font-weight: 600;">
          ADC: <span id="adcModeText" style="margin-left:4px;">Detecting...</span>
        </div>
        <div id="stateBadge" class="badge badge-static">
          <span class="dot"></span>
          <span id="stateText">STATIC BASELINE</span>
        </div>
        <div id="otaBadge" class="badge" style="background: rgba(168,85,247,0.15); border-color: rgba(168,85,247,0.3); color: #c084fc; font-weight: 600;">
          OTA: <span id="otaModeText" style="margin-left:4px;">Auto 15m (v1.1.0)</span>
        </div>
        <div class="badge" style="background: rgba(255,255,255,0.06); border-color: rgba(255,255,255,0.1); color: var(--text-muted);">
          NTP: <span id="clockTime" style="color:#fff; margin-left:4px;">--:--:--</span>
        </div>
      </div>
    </header>

    <!-- Tap Open Compensation Alert Banner -->
    <div id="compBanner" class="comp-banner" style="display: none;">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/></svg>
      <div>
        <strong>Dynamic Pressure Compensation Active:</strong> Tap discharge detected. Water level is frozen at the pre-flow static baseline (<span id="frozenLevel">--</span>%) to prevent false low-reading artifacts.
      </div>
    </div>

    <!-- Main Live Visualization Grid -->
    <div class="grid">
      <!-- Water Tank Column -->
      <div class="card tank-container">
        <div class="card-title" style="width: 100%;">
          <span>Tank Reservoir</span>
          <span id="tankCapText" style="font-size: 0.8rem; color: var(--accent-cyan);">1000 L</span>
        </div>
        <div class="tank-glass">
          <div class="tank-graduations">
            <span>100%</span>
            <span>75%</span>
            <span>50%</span>
            <span>25%</span>
            <span>0%</span>
          </div>
          <div id="waterFill" class="water-fill" style="height: 0%;">
            <div class="water-wave"></div>
          </div>
        </div>
        <div class="tank-value-display">
          <div class="big-pct"><span id="levelPct">--</span><span style="font-size: 1.5rem;">%</span></div>
          <div class="liters-label"><span id="volumeL">--</span> Liters Available</div>
        </div>
      </div>

      <!-- Telemetry & Live Pressure Dynamics Chart -->
      <div class="card">
        <div class="card-title">
          <span>Real-time Hydraulic Metrics</span>
          <span style="font-size: 0.8rem; color: var(--text-muted);">Sampling @ 10Hz</span>
        </div>

        <div class="metrics-row">
          <div class="metric-card">
            <div class="metric-label">Gauge Pressure</div>
            <div class="metric-val"><span id="gaugePressure">--</span><span class="metric-unit">kPa</span></div>
          </div>
          <div class="metric-card">
            <div class="metric-label">Equivalent Head</div>
            <div class="metric-val"><span id="headM">--</span><span class="metric-unit">m</span></div>
          </div>
          <div class="metric-card">
            <div class="metric-label">Sensor Output</div>
            <div class="metric-val"><span id="sensorV">--</span><span class="metric-unit">V</span></div>
          </div>
          <div class="metric-card">
            <div class="metric-label">Rate (dP/dt)</div>
            <div class="metric-val"><span id="dPdt">0.00</span><span class="metric-unit">kPa/s</span></div>
          </div>
        </div>

        <div class="chart-container">
          <canvas id="pressureChart"></canvas>
        </div>
      </div>
    </div>

    <!-- Calibration & Synchronization Settings -->
    <div class="settings-grid">
      <!-- Quick Calibration Card -->
      <div class="card">
        <div class="card-title">
          <span>Two-Point Hydrostatic Calibration</span>
        </div>
        <p class="note-text" style="margin-bottom: 1rem;">
          Calibrate directly at the tap head without climbing to the rooftop tank. Perform once when tank is empty, and once when tank is full.
        </p>

        <div class="metrics-row" style="margin-bottom: 1rem;">
          <div class="metric-card">
            <div class="metric-label">Calibrated EMPTY (0%)</div>
            <div class="metric-val"><span id="valPEmpty">--</span><span class="metric-unit">kPa</span></div>
          </div>
          <div class="metric-card">
            <div class="metric-label">Calibrated FULL (100%)</div>
            <div class="metric-val"><span id="valPFull">--</span><span class="metric-unit">kPa</span></div>
          </div>
        </div>

        <div class="btn-group">
          <button class="btn btn-warning" onclick="calibrateState('empty')">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="8" y1="12" x2="16" y2="12"/></svg>
            Save Current as EMPTY (0%)
          </button>
          <button class="btn btn-success" onclick="calibrateState('full')">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="16"/><line x1="8" y1="12" x2="16" y2="12"/></svg>
            Save Current as FULL (100%)
          </button>
        </div>

        <hr style="border: 0; border-top: 1px solid var(--card-border); margin: 1.25rem 0;">

        <div style="display: flex; gap: 1rem;">
          <div class="form-group" style="flex: 1;">
            <label>Manual Empty kPa</label>
            <input type="number" step="0.1" id="inputEmptyKpa">
          </div>
          <div class="form-group" style="flex: 1;">
            <label>Manual Full kPa</label>
            <input type="number" step="0.1" id="inputFullKpa">
          </div>
        </div>
        <button class="btn btn-secondary" onclick="saveManualCalibration()">Save Custom Values</button>
      </div>

      <!-- Tank Parameters & Google Sheets Sync -->
      <div class="card">
        <div class="card-title">
          <span>Tank Dimensions & Google Sheets Sync</span>
        </div>

        <div style="display: flex; gap: 1rem;">
          <div class="form-group" style="flex: 1;">
            <label>Tank Height / Depth (cm)</label>
            <input type="number" id="inputTankDepth" value="122">
          </div>
          <div class="form-group" style="flex: 1;">
            <label>Tank Capacity (Liters)</label>
            <input type="number" id="inputTankCap" value="1000">
          </div>
        </div>

        <div class="form-group">
          <label>Google Apps Script Web App URL</label>
          <input type="text" id="inputScriptUrl" placeholder="https://script.google.com/macros/s/.../exec">
        </div>

        <div style="display: flex; justify-content: space-between; align-items: center; margin-top: 1rem;">
          <div>
            <div class="note-text">Next Synchronized 1-Min Log:</div>
            <strong id="nextLogTime" style="font-size: 0.9rem; color: var(--accent-cyan);">--</strong>
          </div>
          <div class="btn-group">
            <button class="btn btn-primary" onclick="saveTankConfig()">Save Settings</button>
            <button class="btn btn-secondary" onclick="triggerSheetsLog()">Log Now (Test)</button>
          </div>
        </div>
      </div>

      <!-- Cloud OTA Auto-Updater (Every 15 Minutes) -->
      <div class="card" style="grid-column: span 2;">
        <div class="card-title">
          <span>Cloud Over-The-Air (OTA) Self-Updater</span>
          <span style="font-size:0.8rem; color:#c084fc; font-weight:600;">Active (Every 15 min)</span>
        </div>
        <p class="note-text" style="margin-bottom: 1rem;">
          Autonomously checks the GitHub repository every 15 minutes for newly compiled firmware binaries, downloads updates, and flashes itself safely via dual-partition OTA.
        </p>
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 1rem; align-items: center; margin-bottom: 1rem;">
          <div>
            <label>Current Firmware</label>
            <div style="font-family:'JetBrains Mono'; color:#38bdf8; font-size:1.1rem; font-weight:600;" id="firmwareVerText">v1.1.0</div>
          </div>
          <div>
            <label>Auto-Check Interval</label>
            <div style="font-family:'JetBrains Mono'; color:#f8fafc; font-size:1.1rem;">Every 15 Minutes</div>
          </div>
          <div>
            <label>Cloud OTA Status</label>
            <div style="font-family:'JetBrains Mono'; color:#10b981; font-size:0.95rem;" id="otaStatusDetail">Up to date</div>
          </div>
        </div>
        <div style="display: flex; gap: 0.75rem; flex-wrap: wrap;">
          <input type="password" id="inputGhToken" placeholder="Optional GitHub Token (PAT) for private repo" style="flex: 2; min-width: 250px;">
          <button class="btn btn-secondary" onclick="saveGithubToken()">Save Token</button>
          <button class="btn btn-primary" onclick="triggerOtaCheckNow()">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21.5 2v6h-6M21.34 15.57a10 10 0 1 1-.57-8.38l5.67-5.67"/></svg>
            Check Updates Now
          </button>
        </div>
      </div>
    </div>

    <!-- 24-Hour Experiment Controls -->
    <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 1.5rem; margin-bottom: 1.5rem;">
      <!-- High-Resolution Dual-Sensor Event Recorder (Sheet 2) -->
      <div class="card" style="border: 1px solid rgba(239, 68, 68, 0.4); box-shadow: 0 0 25px rgba(239, 68, 68, 0.1);">
        <div class="card-title">
          <div style="display: flex; align-items: center; gap: 0.5rem;">
            <span id="recDot" style="width: 10px; height: 10px; border-radius: 50%; background: #ef4444; display: inline-block;"></span>
            <span>High-Resolution Event Capture (10 Hz)</span>
          </div>
          <span style="font-size: 0.72rem; color: #f43f5e; font-weight: 700; background: rgba(244,63,94,0.15); padding: 3px 8px; border-radius: 6px;">SHEET 2 ONLY</span>
        </div>
        <p class="note-text" style="margin-bottom: 0.75rem;">
          Captures transient dynamics for <strong>both sensors</strong> simultaneously at <strong>10 Hz (100 ms)</strong> into ESP32 Flash, then streams in chunks to <strong>Sheet 2 ('HighRes_Event_Logs')</strong> with unique event color-coding.
        </p>

        <div style="display: flex; gap: 0.75rem; margin-bottom: 0.5rem;">
          <div style="flex: 2;">
            <label>Event Name</label>
            <input type="text" id="highResEventName" placeholder="e.g., Full Tap Turn 100%, Shower Pulse">
          </div>
          <div style="flex: 1;">
            <label>Duration (Sec)</label>
            <input type="number" id="highResDurationSec" value="30" min="5" max="600">
          </div>
        </div>

        <!-- Duration Preset Pills -->
        <div style="display: flex; gap: 0.4rem; margin-bottom: 0.75rem; flex-wrap: wrap;">
          <button class="btn btn-secondary" style="padding: 0.25rem 0.55rem; font-size: 0.75rem;" onclick="setEventDuration(15)">15s</button>
          <button class="btn btn-secondary" style="padding: 0.25rem 0.55rem; font-size: 0.75rem;" onclick="setEventDuration(30)">30s</button>
          <button class="btn btn-secondary" style="padding: 0.25rem 0.55rem; font-size: 0.75rem;" onclick="setEventDuration(60)">1 min</button>
          <button class="btn btn-secondary" style="padding: 0.25rem 0.55rem; font-size: 0.75rem;" onclick="setEventDuration(120)">2 min</button>
          <button class="btn btn-secondary" style="padding: 0.25rem 0.55rem; font-size: 0.75rem;" onclick="setEventDuration(300)">5 min</button>
        </div>

        <!-- Live Event Recording Monitor Box -->
        <div id="eventLiveBox" style="background: rgba(0,0,0,0.35); border: 1px solid var(--card-border); border-radius: 12px; padding: 0.75rem; margin-bottom: 0.75rem;">
          <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 0.4rem;">
            <span style="font-size: 0.78rem; color: var(--text-muted);">Event Engine:</span>
            <strong id="eventLiveStatus" style="font-size: 0.82rem; color: #10b981;">Ready to Record</strong>
          </div>
          <div style="display: grid; grid-template-columns: repeat(3, 1fr); gap: 0.4rem; text-align: center;">
            <div style="background: rgba(255,255,255,0.04); padding: 0.4rem; border-radius: 8px;">
              <div style="font-size: 0.68rem; color: var(--text-muted);">Elapsed / Target</div>
              <div id="eventElapsedText" style="font-family:'JetBrains Mono'; font-weight:600; color:#38bdf8; font-size:0.88rem;">0.0s / 30s</div>
            </div>
            <div style="background: rgba(255,255,255,0.04); padding: 0.4rem; border-radius: 8px;">
              <div style="font-size: 0.68rem; color: var(--text-muted);">Flash Samples</div>
              <div id="eventSamplesText" style="font-family:'JetBrains Mono'; font-weight:600; color:#c084fc; font-size:0.88rem;">0 @ 10Hz</div>
            </div>
            <div style="background: rgba(255,255,255,0.04); padding: 0.4rem; border-radius: 8px;">
              <div style="font-size: 0.68rem; color: var(--text-muted);">Rooftop Ultrasound</div>
              <div id="eventRooftopText" style="font-family:'JetBrains Mono'; font-weight:600; color:#34d399; font-size:0.88rem;">-- cm</div>
            </div>
          </div>
          <!-- Upload Progress Bar (visible during chunk streaming) -->
          <div id="eventUploadProgressContainer" style="display:none; margin-top: 0.6rem;">
            <div style="display: flex; justify-content: space-between; font-size: 0.75rem; color: var(--text-muted); margin-bottom: 3px;">
              <span>Streaming Chunks to Sheet 2...</span>
              <span id="eventUploadPct">0%</span>
            </div>
            <div style="width: 100%; height: 6px; background: rgba(255,255,255,0.1); border-radius: 3px; overflow: hidden;">
              <div id="eventUploadBar" style="width: 0%; height: 100%; background: linear-gradient(90deg, #38bdf8, #10b981); transition: width 0.3s;"></div>
            </div>
          </div>
        </div>

        <div class="btn-group">
          <button id="btnStartEvent" class="btn btn-danger" onclick="startHighResEvent()">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="8"/></svg>
            Record High-Res Event (10 Hz)
          </button>
          <button id="btnStopEvent" class="btn btn-secondary" onclick="stopHighResEvent()" style="display:none;">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor"><rect x="6" y="6" width="12" height="12"/></svg>
            Stop & Stream to Sheet 2
          </button>
        </div>
      </div>

      <!-- Start 24-Hour Monitoring Experiment -->
      <div class="card">
        <div class="card-title">
          <span>Start 24-Hour Monitoring Experiment</span>
        </div>
        <p class="note-text" style="margin-bottom: 1rem;">
          Wipe all previous test logs from Google Sheets and begin the clean 24-hour synchronized recording now.
        </p>
        <div style="margin-top: 1.5rem;">
          <button class="btn btn-danger" onclick="clearSheetData()">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>
            Clear Sheet & Start 24-Hr Experiment
          </button>
        </div>
      </div>
    </div>
  </div>

  <div id="toast" class="toast">Action completed successfully</div>

  <script>
    let chart;
    const chartTimeWindow = 30; // 30 seconds history
    const labels = [];
    const pressureData = [];
    let isFlowingPrev = false;

    // Initialize Chart
    function initChart() {
      const ctx = document.getElementById('pressureChart').getContext('2d');
      chart = new Chart(ctx, {
        type: 'line',
        data: {
          labels: labels,
          datasets: [{
            label: 'Gauge Pressure (kPa)',
            data: pressureData,
            borderColor: '#38bdf8',
            backgroundColor: 'rgba(56, 189, 248, 0.08)',
            borderWidth: 2,
            tension: 0.25,
            fill: true,
            pointRadius: 0
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          plugins: { legend: { display: false } },
          scales: {
            x: {
              display: false
            },
            y: {
              suggestedMin: 0,
              suggestedMax: 65,
              grid: { color: 'rgba(255, 255, 255, 0.05)' },
              ticks: { color: '#94a3b8', font: { family: 'JetBrains Mono', size: 10 } }
            }
          },
          animation: { duration: 0 }
        }
      });
    }

    // Fetch Telemetry Data from ESP32 REST API
    async function fetchStatus() {
      try {
        const res = await fetch('/api/status');
        if (!res.ok) return;
        const d = await res.json();

        // Update Tank Visualizer
        const pct = Math.max(0, Math.min(100, d.level_pct));
        document.getElementById('levelPct').innerText = pct.toFixed(1);
        document.getElementById('waterFill').style.height = pct + '%';
        document.getElementById('volumeL').innerText = Math.round(d.volume_liters);
        document.getElementById('tankCapText').innerText = d.tank_capacity + ' L';

        // Update Metrics
        if (d.adc_mode) {
          document.getElementById('adcModeText').innerText = d.adc_mode;
        }
        if (d.ota_status) {
          document.getElementById('otaStatusDetail').innerText = d.ota_status;
          document.getElementById('otaModeText').innerText = d.ota_status;
        }
        if (d.firmware_version) {
          document.getElementById('firmwareVerText').innerText = 'v' + d.firmware_version;
        }
        document.getElementById('gaugePressure').innerText = d.pressure_kpa.toFixed(2);
        document.getElementById('headM').innerText = d.head_m.toFixed(2);
        document.getElementById('sensorV').innerText = d.voltage.toFixed(3);
        document.getElementById('dPdt').innerText = (d.dp_dt > 0 ? '+' : '') + d.dp_dt.toFixed(2);
        document.getElementById('clockTime').innerText = d.time_str || '--:--:--';
        document.getElementById('nextLogTime').innerText = d.next_log_slot || '--';

        // Calibration Display
        document.getElementById('valPEmpty').innerText = d.p_empty.toFixed(1);
        document.getElementById('valPFull').innerText = d.p_full.toFixed(1);

        // State Badge
        const badge = document.getElementById('stateBadge');
        const stateText = document.getElementById('stateText');
        const compBanner = document.getElementById('compBanner');

        if (d.tap_state === 'TAP_OPEN') {
          badge.className = 'badge badge-flowing';
          stateText.innerText = 'TAP OPEN (DYNAMIC FLOW)';
          compBanner.style.display = 'flex';
          document.getElementById('frozenLevel').innerText = d.frozen_pct.toFixed(1);
        } else if (d.tap_state === 'SETTLING') {
          badge.className = 'badge badge-settling';
          stateText.innerText = 'SETTLING (HAMMER REJECTION)';
          compBanner.style.display = 'none';
        } else {
          badge.className = 'badge badge-static';
          stateText.innerText = 'STATIC BASELINE';
          compBanner.style.display = 'none';
        }

        // Update Chart
        const now = new Date().toLocaleTimeString();
        labels.push(now);
        pressureData.push(d.pressure_kpa);
        if (labels.length > 50) {
          labels.shift();
          pressureData.shift();
        }
        chart.update();

      } catch (err) {
        console.error('Fetch error:', err);
      }
    }

    // Calibrate Empty or Full
    async function calibrateState(action) {
      if (!confirm(`Are you sure you want to calibrate current pressure as ${action.toUpperCase()} (${action === 'empty' ? '0%' : '100%'})?`)) return;
      try {
        const res = await fetch(`/api/calibrate?action=${action}`, { method: 'POST' });
        const d = await res.json();
        showToast(d.message || 'Calibration saved!');
        fetchStatus();
      } catch (e) {
        showToast('Error saving calibration');
      }
    }

    // Save Manual Calibration
    async function saveManualCalibration() {
      const pEmp = parseFloat(document.getElementById('inputEmptyKpa').value);
      const pFul = parseFloat(document.getElementById('inputFullKpa').value);
      if (isNaN(pEmp) || isNaN(pFul) || pFul <= pEmp) {
        alert('Invalid calibration values: Full kPa must be greater than Empty kPa');
        return;
      }
      try {
        const res = await fetch(`/api/calibrate?action=manual&p_empty=${pEmp}&p_full=${pFul}`, { method: 'POST' });
        const d = await res.json();
        showToast(d.message || 'Manual calibration updated');
        fetchStatus();
      } catch (e) {
        showToast('Failed to save values');
      }
    }

    // Save Tank & Script Configuration
    async function saveTankConfig() {
      const depth = document.getElementById('inputTankDepth').value;
      const cap = document.getElementById('inputTankCap').value;
      const url = encodeURIComponent(document.getElementById('inputScriptUrl').value);
      try {
        const res = await fetch(`/api/settings?tank_depth=${depth}&tank_cap=${cap}&script_url=${url}`, { method: 'POST' });
        const d = await res.json();
        showToast(d.message || 'Settings saved');
      } catch (e) {
        showToast('Failed to save settings');
      }
    }

    // Manual Google Sheets Log Trigger
    async function triggerSheetsLog() {
      showToast('Initiating test sync with Google Sheets...');
      try {
        const res = await fetch('/api/log_sheets', { method: 'POST' });
        const d = await res.json();
        showToast(d.message || 'Sync response received');
      } catch (e) {
        showToast('Sync request failed');
      }
    }

    // Trigger Manual Test from localhost dashboard
    async function triggerManualTest(start) {
      let testName = "";
      let dur = 5;
      if (start) {
        testName = document.getElementById('manualTestName').value;
        dur = document.getElementById('manualTestDuration').value;
        if (!testName || testName.trim() === '') {
          alert("Please enter a Test Name (e.g. 'Kitchen Tap 50% flow')");
          return;
        }
      } else {
        testName = "STOP";
      }

      const scriptUrl = document.getElementById('inputScriptUrl').value;
      if (!scriptUrl || scriptUrl.length < 10) {
        alert("Please save a valid Google Apps Script Web App URL first!");
        return;
      }

      try {
        showToast(start ? "Tagging Manual Test in Sheets..." : "Stopping Manual Test Tag...");
        const url = scriptUrl + '?action=set_manual_test&test_name=' + encodeURIComponent(testName) + '&duration_min=' + dur;
        await fetch(url, { mode: 'no-cors' });
        showToast(start ? ("Test '" + testName + "' marked in Sheets!") : "Manual test tagging stopped.");
      } catch (e) {
        showToast("Error updating manual test tag");
      }
    }

    // Clear All Sheet Data from localhost dashboard
    async function clearSheetData() {
      if (!confirm("Are you sure you want to EMPTY all previous data in Google Sheets and start the 24-hr experiment from scratch?")) return;
      const scriptUrl = document.getElementById('inputScriptUrl').value;
      if (!scriptUrl || scriptUrl.length < 10) {
        alert("Please save a valid Google Apps Script Web App URL first!");
        return;
      }

      try {
        showToast("Sending wipe command to Google Sheets...");
        const url = scriptUrl + '?action=clear_sheet';
        await fetch(url, { mode: 'no-cors' });
        showToast("Google Sheet reset command sent successfully!");
      } catch (e) {
        showToast("Error clearing sheet");
      }
    }

    async function triggerOtaCheckNow() {
      showToast("Contacting GitHub for new firmware update...");
      try {
        const res = await fetch('/api/ota_check', { method: 'POST' });
        const d = await res.json();
        showToast(d.message || "OTA check completed");
        fetchStatus();
      } catch(e) {
        showToast("Error checking updates: " + e.message);
      }
    }

    async function saveGithubToken() {
      const token = document.getElementById('inputGhToken').value.trim();
      const formData = new FormData();
      formData.append('gh_token', token);
      try {
        const res = await fetch('/api/settings', { method: 'POST', body: formData });
        if (res.ok) {
          showToast("GitHub Token saved to ESP32 Flash!");
        } else {
          showToast("Error saving token");
        }
      } catch(e) {
        showToast("Error saving token: " + e.message);
      }
    }

    // High-Resolution 10 Hz Event Capture Controller (Sheet 2)
    let eventColorCounter = 0;

    function setEventDuration(sec) {
      document.getElementById('highResDurationSec').value = sec;
      const nameEl = document.getElementById('highResEventName');
      if (!nameEl.value || nameEl.value.trim() === '') {
        nameEl.value = 'Tap Turn Test (' + sec + 's)';
      }
    }

    async function startHighResEvent() {
      const name = document.getElementById('highResEventName').value.trim() || 'Tap Transient Event';
      const durSec = parseFloat(document.getElementById('highResDurationSec').value) || 30;

      const formData = new FormData();
      formData.append('name', name);
      formData.append('duration_sec', durSec);
      formData.append('color_idx', eventColorCounter++);

      showToast('Starting High-Resolution 10 Hz Event Capture...');
      try {
        const res = await fetch('/api/event/start', { method: 'POST', body: formData });
        const data = await res.json();
        if (res.ok) {
          showToast('🔴 Recording started: 10 samples/sec into Flash!');
          document.getElementById('btnStartEvent').style.display = 'none';
          document.getElementById('btnStopEvent').style.display = 'inline-flex';
          pollEventStatus();
        } else {
          showToast('Error: ' + data.message);
        }
      } catch(e) {
        showToast('Failed to start event: ' + e.message);
      }
    }

    async function stopHighResEvent() {
      showToast('Stopping recording. Streaming chunks to Sheet 2...');
      try {
        const res = await fetch('/api/event/stop', { method: 'POST' });
        const data = await res.json();
        showToast(data.message || 'Streaming chunks to Sheet 2...');
        pollEventStatus();
      } catch(e) {
        showToast('Error stopping event: ' + e.message);
      }
    }

    async function pollEventStatus() {
      try {
        const res = await fetch('/api/event/status');
        if (!res.ok) return;
        const d = await res.json();

        const statusEl = document.getElementById('eventLiveStatus');
        const elapsedEl = document.getElementById('eventElapsedText');
        const samplesEl = document.getElementById('eventSamplesText');
        const rooftopEl = document.getElementById('eventRooftopText');
        const progContainer = document.getElementById('eventUploadProgressContainer');
        const uploadBar = document.getElementById('eventUploadBar');
        const uploadPct = document.getElementById('eventUploadPct');
        const recDot = document.getElementById('recDot');
        const btnStart = document.getElementById('btnStartEvent');
        const btnStop = document.getElementById('btnStopEvent');

        if (d.recording) {
          statusEl.innerText = '🔴 RECORDING (10 Hz High-Speed)...';
          statusEl.style.color = '#ef4444';
          recDot.style.background = '#ef4444';
          recDot.style.animation = 'pulseAlert 1s infinite';
          elapsedEl.innerText = d.elapsed_sec.toFixed(1) + 's / ' + d.duration_sec.toFixed(0) + 's';
          samplesEl.innerText = d.samples + ' @ 10Hz';
          rooftopEl.innerText = d.rooftop_dist.toFixed(1) + ' cm (' + d.rooftop_level.toFixed(1) + '%)';
          progContainer.style.display = 'none';
          btnStart.style.display = 'none';
          btnStop.style.display = 'inline-flex';
          setTimeout(pollEventStatus, 250); // Fast 250ms polling during active recording
        } else if (d.uploading) {
          statusEl.innerText = '🚀 STREAMING TO SHEET 2...';
          statusEl.style.color = '#38bdf8';
          recDot.style.background = '#38bdf8';
          recDot.style.animation = 'none';
          progContainer.style.display = 'block';
          uploadBar.style.width = d.upload_pct + '%';
          uploadPct.innerText = d.upload_pct + '%';
          elapsedEl.innerText = 'Uploaded ' + d.upload_pct + '%';
          samplesEl.innerText = d.samples + ' samples';
          btnStart.style.display = 'none';
          btnStop.style.display = 'none';
          setTimeout(pollEventStatus, 500); // 500ms polling during chunk streaming
        } else {
          statusEl.innerText = d.upload_status || 'Ready to Record';
          statusEl.style.color = '#10b981';
          recDot.style.background = '#10b981';
          recDot.style.animation = 'none';
          progContainer.style.display = 'none';
          btnStart.style.display = 'inline-flex';
          btnStop.style.display = 'none';
          if (d.rooftop_dist > 0) {
            rooftopEl.innerText = d.rooftop_dist.toFixed(1) + ' cm (' + d.rooftop_level.toFixed(1) + '%)';
          }
        }
      } catch(e) {}
    }

    function showToast(msg) {
      const t = document.getElementById('toast');
      t.innerText = msg;
      t.classList.add('show');
      setTimeout(() => t.classList.remove('show'), 3500);
    }

    window.onload = () => {
      initChart();
      fetchStatus();
      setInterval(fetchStatus, 1000); // Poll telemetry every 1 sec
      pollEventStatus();
    };
  </script>
</body>
</html>
)rawliteral";

#endif // WEB_DASHBOARD_H
