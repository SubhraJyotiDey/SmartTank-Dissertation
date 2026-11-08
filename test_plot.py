import os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# Create output directory
OUT_DIR = r"A:\Dissertation\code\plots\highres_events"
os.makedirs(OUT_DIR, exist_ok=True)

df = pd.read_csv('data/highres_events_sheet2.csv')

# Test on EVT_2470 (Extreme Joukowsky water hammer)
g = df[df['Event ID'] == 'EVT_2470'].copy()

# Set up matplotlib style
plt.rcParams['font.family'] = 'DejaVu Sans'
plt.rcParams['font.size'] = 9.5
plt.rcParams['axes.titlesize'] = 11
plt.rcParams['axes.titleweight'] = 'bold'
plt.rcParams['axes.labelsize'] = 10
plt.rcParams['axes.labelweight'] = 'bold'
plt.rcParams['figure.titlesize'] = 13
plt.rcParams['figure.titleweight'] = 'bold'

fig = plt.figure(figsize=(14, 10.5), dpi=300)
gs = fig.add_gridspec(3, 1, height_ratios=[1.2, 1.0, 0.9], hspace=0.28)

ax1 = fig.add_subplot(gs[0, 0])
ax2 = fig.add_subplot(gs[1, 0], sharex=ax1)
ax3 = fig.add_subplot(gs[2, 0], sharex=ax1)

t = g['Elapsed Time (s)'].values
p = g['Gauge Pressure (kPa)'].values
dp = g['dP/dt (kPa/s)'].values
states = g['Tap Flow State'].values
u_truth = g['Ultrasonic Ground Truth (%)'].values
p_est = g['Pressure Est. Level (%)'].values
vol = g['Cumulative Volume (L)'].values
flow = g['Flow Rate (L/s)'].values

# Baseline static pressure
static_mask = states == 'STATIC'
p_base = p[static_mask][0] if np.any(static_mask) else p[0]

# Compensated level calculation
p_empty = -3.96
p_full = 57.53
naive_level = np.clip(1.6262 * p + 6.4417, 0.0, 100.0)

# Dual-node compensated level
comp_level = np.zeros_like(p)
current_comp = u_truth[0]
for i in range(len(g)):
    if states[i] == 'STATIC':
        current_comp = u_truth[i]
        comp_level[i] = current_comp
    else:
        # Frozen baseline minus integrated volume loss
        loss_pct = (vol[i] / 1000.0) * 100.0
        comp_level[i] = current_comp - loss_pct

# -----------------------------------------------------------------------------
# Background State Shading on all subplots
# -----------------------------------------------------------------------------
state_colors = {
    'STATIC': ('#dcfce7', 'Static Hydrostatic'),
    'TAP_OPEN': ('#fef3c7', 'Dynamic Tap Open'),
    'SETTLING': ('#ede9fe', 'Joukowsky Settling')
}

# Find contiguous state segments
segments = []
cur_state = states[0]
start_idx = 0
for i in range(1, len(states)):
    if states[i] != cur_state:
        segments.append((cur_state, t[start_idx], t[i-1]))
        cur_state = states[i]
        start_idx = i
segments.append((cur_state, t[start_idx], t[-1]))

for ax in [ax1, ax2, ax3]:
    for st, t0, t1 in segments:
        color, _ = state_colors.get(st, ('#f1f5f9', st))
        ax.axvspan(t0, t1, color=color, alpha=0.55, zorder=0)

# -----------------------------------------------------------------------------
# Panel 1: Gauge Pressure & Equivalent Water Head
# -----------------------------------------------------------------------------
ax1.plot(t, p, color='#1d4ed8', linewidth=2.0, label='Gauge Pressure (Tap Node)', zorder=4)
ax1.axhline(p_base, color='#64748b', linestyle='--', linewidth=1.3, label=f'Static Baseline P₀ = {p_base:.2f} kPa', zorder=2)

# Head right axis (P = rho * g * H => H = P / 9.80665)
ax1_r = ax1.twinx()
ax1_r.set_ylabel('Equivalent Head H_w (m H₂O)', color='#1e3a8a', fontweight='bold')
ax1_r.tick_params(axis='y', labelcolor='#1e3a8a')
p_lims = ax1.get_ylim()
ax1_r.set_ylim(p_lims[0] / 9.80665, p_lims[1] / 9.80665)

# Annotations for P_min and P_max
p_min = np.min(p)
t_min = t[np.argmin(p)]
delta_drop = p_base - p_min

p_max = np.max(p)
t_max = t[np.argmax(p)]
delta_surge = p_max - p_base

ax1.scatter([t_min], [p_min], color='#dc2626', s=55, zorder=6, edgecolors='black')
ax1.annotate(f'Friction Trough:\n{p_min:.1f} kPa (-{delta_drop:.1f} kPa)',
             xy=(t_min, p_min), xytext=(t_min + 0.3, p_min + 20),
             arrowprops=dict(facecolor='#dc2626', shrink=0.08, width=1.5, headwidth=6),
             fontsize=8.5, fontweight='bold', color='#991b1b',
             bbox=dict(boxstyle='round,pad=0.25', facecolor='#fee2e2', edgecolor='#dc2626', alpha=0.9))

if delta_surge > 2.0:
    ax1.scatter([t_max], [p_max], color='#7c3aed', s=65, zorder=6, edgecolors='black')
    ax1.annotate(f'Water Hammer Peak:\n{p_max:.1f} kPa (+{delta_surge:.1f} kPa)',
                 xy=(t_max, p_max), xytext=(t_max - 2.8, p_max - 25),
                 arrowprops=dict(facecolor='#7c3aed', shrink=0.08, width=1.5, headwidth=6),
                 fontsize=8.5, fontweight='bold', color='#581c87',
                 bbox=dict(boxstyle='round,pad=0.25', facecolor='#f3e8ff', edgecolor='#7c3aed', alpha=0.9))

ax1.set_ylabel('Gauge Pressure P (kPa)', color='#1d4ed8', fontweight='bold')
ax1.tick_params(axis='y', labelcolor='#1d4ed8')
ax1.grid(True, linestyle=':', alpha=0.6)
ax1.set_title('Hydraulic Dynamic Transient & Valve Closure Water Hammer (Joukowsky Surge)', pad=6)

# Custom legend for ax1 with state patches
patches = [
    mpatches.Patch(facecolor='#dcfce7', edgecolor='#86efac', label='STATIC (Quiescent)'),
    mpatches.Patch(facecolor='#fef3c7', edgecolor='#fde047', label='TAP OPEN (Discharge)'),
    mpatches.Patch(facecolor='#ede9fe', edgecolor='#d8b4fe', label='SETTLING (Acoustic Damping)'),
]
lines1, labels1 = ax1.get_legend_handles_labels()
ax1.legend(handles=lines1 + patches, loc='upper right', framealpha=0.9, fontsize=8)

# -----------------------------------------------------------------------------
# Panel 2: Level Comparison (Ground Truth vs Uncompensated vs Compensated)
# -----------------------------------------------------------------------------
ax2.plot(t, u_truth, color='#059669', linewidth=2.4, label='Ultrasonic Ground Truth (Node 2, 6.0 cm Full Ref)', zorder=5)
ax2.plot(t, naive_level, color='#dc2626', linestyle='--', linewidth=1.8, label='Naive Uncompensated Pressure Level (False Plunge)', zorder=4)
ax2.plot(t, comp_level, color='#2563eb', linewidth=2.0, label='Compensated Dual-Node Estimate (Algorithm Active)', zorder=6)

# Fill error between naive and truth
ax2.fill_between(t, u_truth, naive_level, color='#fca5a5', alpha=0.35, label='Naive Hydrodynamic Error Region', zorder=2)

ax2.set_ylabel('Water Level (%)', fontweight='bold')
ax2.set_ylim(-5, 110)
ax2.grid(True, linestyle=':', alpha=0.6)
ax2.set_title('Dual-Node Level Accuracy vs Naive Single-Node Hydrodynamic Vulnerability', pad=6)

# Secondary Depth axis (Level % * 122.0 cm / 100)
ax2_r = ax2.twinx()
ax2_r.set_ylabel('Water Depth in Tank (cm)', color='#065f46', fontweight='bold')
ax2_r.tick_params(axis='y', labelcolor='#065f46')
ax2_r.set_ylim(-5 * 1.22, 110 * 1.22)
ax2.legend(loc='lower left', framealpha=0.9, fontsize=8)

# -----------------------------------------------------------------------------
# Panel 3: dP/dt Derivative & Volumetric Discharge
# -----------------------------------------------------------------------------
ax3.plot(t, dp, color='#7c3aed', linewidth=1.5, label='dP/dt (Pressure Rate of Change)', zorder=4)
ax3.axhline(0, color='#94a3b8', linestyle='-', linewidth=0.8)
ax3.axhline(-2.2, color='#ef4444', linestyle=':', linewidth=1.2, label='Opening Threshold (-2.2 kPa/s)')
ax3.axhline(+1.8, color='#10b981', linestyle=':', linewidth=1.2, label='Closure Surge Threshold (+1.8 kPa/s)')

ax3.set_ylabel('dP/dt (kPa/s)', color='#7c3aed', fontweight='bold')
ax3.tick_params(axis='y', labelcolor='#7c3aed')
ax3.grid(True, linestyle=':', alpha=0.6)
ax3.set_xlabel('Elapsed Time (s)', fontweight='bold')
ax3.set_title('Hydraulic Derivatives & Volumetric Outflow Integration', pad=6)

# Twin axis for Cumulative Volume
ax3_r = ax3.twinx()
ax3_r.plot(t, vol, color='#0d9488', linewidth=2.0, linestyle='-.', label='Cumulative Discharged Volume (L)')
ax3_r.set_ylabel('Discharged Volume (L)', color='#0d9488', fontweight='bold')
ax3_r.tick_params(axis='y', labelcolor='#0d9488')

lines3_l, labels3_l = ax3.get_legend_handles_labels()
lines3_r, labels3_r = ax3_r.get_legend_handles_labels()
ax3.legend(lines3_l + lines3_r, labels3_l + labels3_r, loc='upper left', framealpha=0.9, fontsize=7.5)

# -----------------------------------------------------------------------------
# Event Title & Dissertation Info Banner
# -----------------------------------------------------------------------------
fig.suptitle(f"HIGH-RESOLUTION HYDRAULIC TRANSIENT ANALYSIS | EVENT 06: Both Taps OPEN (m)\n"
             f"Dual-Node Hydrodynamic Calibration Benchmark | 10 Hz Telemetry (100 ms Sample Rate)",
             fontsize=12, y=0.985)

out_file = os.path.join(OUT_DIR, "test_event_06.png")
plt.savefig(out_file, dpi=300, bbox_inches='tight')
plt.close()
print(f"Saved test plot to: {out_file}")
