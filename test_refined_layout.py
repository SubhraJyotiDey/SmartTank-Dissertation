"""
Test refined publication layout on EVT_2470 and EVT_3598
"""
import os
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

OUT_DIR = r"plots/highres_events"
os.makedirs(OUT_DIR, exist_ok=True)

df = pd.read_csv('data/highres_events_sheet2.csv')

plt.rcParams['font.family'] = 'DejaVu Sans'
plt.rcParams['font.size'] = 9.0
plt.rcParams['axes.titlesize'] = 10.5
plt.rcParams['axes.titleweight'] = 'bold'
plt.rcParams['axes.labelsize'] = 9.5
plt.rcParams['axes.labelweight'] = 'bold'
plt.rcParams['figure.titlesize'] = 12.0
plt.rcParams['figure.titleweight'] = 'bold'

TANK_DEPTH_CM = 122.0
TANK_CAPACITY_L = 1000.0

def render_event_figure(idx, eid, ename, group, out_path):
    t = group['Elapsed Time (s)'].values
    p = group['Gauge Pressure (kPa)'].values
    dp = group['dP/dt (kPa/s)'].values
    states = group['Tap Flow State'].values
    u_truth = group['Ultrasonic Ground Truth (%)'].values
    u_dist = group['Ultrasonic Distance (cm)'].values
    vol = group['Cumulative Volume (L)'].values
    flow = group['Flow Rate (L/s)'].values

    duration = t[-1] - t[0]
    n_samples = len(group)

    # Static baseline P0
    static_mask = (states == 'STATIC')
    if np.any(static_mask):
        p_base = float(np.median(p[static_mask][:15])) if np.sum(static_mask) >= 5 else float(p[static_mask][0])
    else:
        p_base = float(p[0])

    naive_level = np.clip(1.6262 * p + 6.4417, 0.0, 100.0)

    # True monotonic volume integration:
    monotonic_vol = np.zeros_like(vol)
    cur_vol = 0.0
    for i in range(1, len(vol)):
        dt = t[i] - t[i-1]
        # integrate flow or delta vol
        d_v = vol[i] - vol[i-1]
        if d_v < -0.005: # reset detected in firmware
            cur_vol += vol[i-1]
        monotonic_vol[i] = cur_vol + vol[i]
    if np.max(monotonic_vol) == 0.0 and np.max(vol) > 0:
        monotonic_vol = vol.copy()
    vol_discharged = float(np.max(monotonic_vol) - np.min(monotonic_vol))

    comp_level = np.zeros_like(p)
    current_comp = float(u_truth[0])
    for i in range(len(group)):
        if states[i] == 'STATIC':
            current_comp = float(u_truth[i])
            comp_level[i] = current_comp
        else:
            loss_pct = (monotonic_vol[i] / TANK_CAPACITY_L) * 100.0
            comp_level[i] = current_comp - loss_pct

    p_min = float(np.min(p))
    t_min = float(t[np.argmin(p)])
    delta_friction_drop = max(0.0, p_base - p_min)

    p_max = float(np.max(p))
    t_max = float(t[np.argmax(p)])
    delta_hammer_surge = max(0.0, p_max - p_base)

    dp_max = float(np.max(dp))
    dp_min = float(np.min(dp))

    u_mean = float(np.mean(u_truth))
    u_dist_mean = float(np.mean(u_dist))

    naive_max_err = float(np.max(np.abs(naive_level - u_truth)))
    comp_max_err = float(np.max(np.abs(comp_level - u_truth)))

    # Count states
    st_counts = pd.Series(states).value_counts().to_dict()
    static_cnt = st_counts.get('STATIC', 0)
    open_cnt = st_counts.get('TAP_OPEN', 0)
    settling_cnt = st_counts.get('SETTLING', 0)

    # Figure Layout: 4 rows (Row 0 = Metadata Cards, Rows 1-3 = Synchronized Subplots)
    fig = plt.figure(figsize=(14.5, 11.2), dpi=300)
    gs = fig.add_gridspec(4, 1, height_ratios=[0.38, 1.25, 1.05, 0.90], hspace=0.28)

    ax_meta = fig.add_subplot(gs[0, 0])
    ax1 = fig.add_subplot(gs[1, 0])
    ax2 = fig.add_subplot(gs[2, 0], sharex=ax1)
    ax3 = fig.add_subplot(gs[3, 0], sharex=ax1)

    # -------------------------------------------------------------------------
    # Row 0: Dedicated Metadata Strip (4 Sleek Cards)
    # -------------------------------------------------------------------------
    ax_meta.axis('off')
    # Draw background box
    meta_box = mpatches.FancyBboxPatch((0.0, 0.0), 1.0, 1.0, boxstyle="round,pad=0.02,rounding_size=0.03",
                                      facecolor='#f8fafc', edgecolor='#cbd5e1', linewidth=1.2, transform=ax_meta.transAxes)
    ax_meta.add_patch(meta_box)

    c1_text = (
        r"$\bf{HYDRAULIC\ BASELINE\ &\ LOSS}$" + "\n"
        f"• Static P₀: {p_base:.2f} kPa ({p_base/9.80665:.2f} m H₂O)\n"
        f"• Friction Trough P_min: {p_min:.2f} kPa\n"
        f"• Dynamic Loss ΔP: -{delta_friction_drop:.2f} kPa (-{delta_friction_drop/9.80665:.2f} m)"
    )
    c2_text = (
        r"$\bf{WATER\ HAMMER\ TRANSIENT}$" + "\n"
        f"• Hammer Peak: {p_max:.2f} kPa ({p_max/9.80665:.2f} m)\n"
        f"• Overpressure Surge ΔP: +{delta_hammer_surge:.2f} kPa\n"
        f"• Derivative dP/dt: {dp_max:+.1f} / {dp_min:+.1f} kPa/s"
    )
    c3_text = (
        r"$\bf{LEVEL\ ACCURACY\ &\ ERROR}$" + "\n"
        f"• Ultrasonic Truth: {u_mean:.1f}% (Dist: {u_dist_mean:.1f} cm)\n"
        f"• Max Naive Error: {naive_max_err:.1f}% ({naive_max_err*1.22:.1f} cm)\n"
        f"• Dual-Node Residual Error: {comp_max_err:.2f}% ({comp_max_err*1.22:.2f} cm)"
    )
    c4_text = (
        r"$\bf{FLOW\ &\ DURATION\ METRICS}$" + "\n"
        f"• Discharge Vol: {vol_discharged:.3f} L (Peak Flow: {np.max(flow):.3f} L/s)\n"
        f"• Duration: {duration:.2f} s | Samples: {n_samples} (10 Hz)\n"
        f"• States: Static {static_cnt}s, Open {open_cnt}s, Settle {settling_cnt}s"
    )

    ax_meta.text(0.02, 0.82, c1_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top', fontfamily='DejaVu Sans')
    ax_meta.text(0.27, 0.82, c2_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top', fontfamily='DejaVu Sans')
    ax_meta.text(0.53, 0.82, c3_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top', fontfamily='DejaVu Sans')
    ax_meta.text(0.78, 0.82, c4_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top', fontfamily='DejaVu Sans')

    # Draw vertical divider lines
    for div_x in [0.25, 0.51, 0.76]:
        ax_meta.plot([div_x, div_x], [0.08, 0.92], color='#cbd5e1', linestyle='-', linewidth=1.0, transform=ax_meta.transAxes)

    # -------------------------------------------------------------------------
    # State Shading on Subplots
    # -------------------------------------------------------------------------
    state_colors = {
        'STATIC': ('#dcfce7', 'STATIC (Quiescent Hydrostatic)'),
        'TAP_OPEN': ('#fef3c7', 'TAP_OPEN (Dynamic Discharge)'),
        'SETTLING': ('#ede9fe', 'SETTLING (Joukowsky Damping)')
    }

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

    # -------------------------------------------------------------------------
    # Panel 1: Gauge Pressure & Equivalent Water Head
    # -------------------------------------------------------------------------
    ax1.plot(t, p, color='#1d4ed8', linewidth=2.0, label='Gauge Pressure P(t) [Bottom Tap Node]', zorder=4)
    ax1.axhline(p_base, color='#475569', linestyle='--', linewidth=1.3,
                label=f'Static Baseline P₀ = {p_base:.2f} kPa ({p_base/9.80665:.2f} m H₂O)', zorder=2)

    ax1_r = ax1.twinx()
    ax1_r.set_ylabel('Piezometric Head H_w (m H₂O)', color='#1e3a8a', fontweight='bold')
    ax1_r.tick_params(axis='y', labelcolor='#1e3a8a')

    # Generous Y-limits
    p_lower = min(0.0, p_min - 6.0)
    p_upper = max(p_base * 1.30, p_max * 1.15 + 10.0)
    ax1.set_ylim(p_lower, p_upper)
    ax1_r.set_ylim(p_lower / 9.80665, p_upper / 9.80665)

    t_span = t[-1] - t[0]
    p_span = p_upper - p_lower

    # Friction Trough Annotation
    x_off_drop = 0.07 * t_span if t_min < (t[0] + 0.65 * t_span) else -0.22 * t_span
    y_off_drop = 0.15 * p_span
    ax1.scatter([t_min], [p_min], color='#dc2626', s=60, zorder=6, edgecolors='black', linewidth=1.2)
    ax1.annotate(f'Friction Trough P_min: {p_min:.2f} kPa\n(ΔP = -{delta_friction_drop:.2f} kPa | -{delta_friction_drop/9.80665:.2f} m Loss)',
                 xy=(t_min, p_min), xytext=(t_min + x_off_drop, p_min + y_off_drop),
                 arrowprops=dict(facecolor='#dc2626', shrink=0.08, width=1.4, headwidth=5),
                 fontsize=8.0, fontweight='bold', color='#991b1b',
                 bbox=dict(boxstyle='round,pad=0.25', facecolor='#fee2e2', edgecolor='#dc2626', alpha=0.92))

    # Water Hammer Annotation
    if delta_hammer_surge >= 2.0:
        x_off_surge = -0.25 * t_span if t_max > (t[0] + 0.40 * t_span) else 0.08 * t_span
        y_off_surge = -0.16 * p_span if p_max > (p_base + 0.55 * p_span) else 0.10 * p_span
        ax1.scatter([t_max], [p_max], color='#7c3aed', s=70, zorder=6, edgecolors='black', linewidth=1.2)
        ax1.annotate(f'Joukowsky Hammer Peak: {p_max:.2f} kPa\n(+{delta_hammer_surge:.2f} kPa Surge | +{delta_hammer_surge/9.80665:.2f} m Surge)',
                     xy=(t_max, p_max), xytext=(t_max + x_off_surge, p_max + y_off_surge),
                     arrowprops=dict(facecolor='#7c3aed', shrink=0.08, width=1.4, headwidth=5),
                     fontsize=8.0, fontweight='bold', color='#581c87',
                     bbox=dict(boxstyle='round,pad=0.25', facecolor='#f3e8ff', edgecolor='#7c3aed', alpha=0.92))

    ax1.set_ylabel('Gauge Pressure P (kPa)', color='#1d4ed8', fontweight='bold')
    ax1.tick_params(axis='y', labelcolor='#1d4ed8')
    ax1.grid(True, linestyle=':', alpha=0.6)
    ax1.set_title('Hydraulic Dynamic Transient & Valve Closure Water Hammer (Piezometric Head Dynamics)', pad=5)

    patches = [
        mpatches.Patch(facecolor='#dcfce7', edgecolor='#86efac', label='STATIC (Hydrostatic)'),
        mpatches.Patch(facecolor='#fef3c7', edgecolor='#fde047', label='TAP_OPEN (Discharge)'),
        mpatches.Patch(facecolor='#ede9fe', edgecolor='#d8b4fe', label='SETTLING (Acoustic Decay)'),
    ]
    lines1, labels1 = ax1.get_legend_handles_labels()
    ax1.legend(handles=lines1 + patches, loc='upper right', framealpha=0.92, fontsize=7.6, ncol=2)

    # -------------------------------------------------------------------------
    # Panel 2: Water Level Comparison
    # -------------------------------------------------------------------------
    ax2.plot(t, u_truth, color='#059669', linewidth=2.5,
             label=f'Ultrasonic Ground Truth (Node 2: Mean = {u_mean:.1f}%, Dist = {u_dist_mean:.1f} cm)', zorder=5)
    ax2.plot(t, naive_level, color='#dc2626', linestyle='--', linewidth=1.8,
             label=f'Naive Uncompensated Pressure Level (Max Error = {naive_max_err:.1f}%)', zorder=4)
    ax2.plot(t, comp_level, color='#2563eb', linewidth=2.0,
             label=f'Compensated Smart Dual-Node Estimate (Max Error = {comp_max_err:.2f}%)', zorder=6)

    ax2.fill_between(t, u_truth, naive_level, color='#fca5a5', alpha=0.38,
                     label=f'Hydrodynamic Failure Zone (Bernoulli Head Drop / Hammer Spike)', zorder=2)

    ax2.set_ylabel('Water Level (%)', fontweight='bold')
    ax2.set_ylim(-5, 115)
    ax2.grid(True, linestyle=':', alpha=0.6)
    ax2.set_title('Dual-Node Surface Tracking Accuracy vs Naive Pressure-Inferred Level Vulnerability', pad=5)

    ax2_r = ax2.twinx()
    ax2_r.set_ylabel('Water Height in Tank (cm)', color='#065f46', fontweight='bold')
    ax2_r.tick_params(axis='y', labelcolor='#065f46')
    ax2_r.set_ylim(-5 * (TANK_DEPTH_CM / 100.0), 115 * (TANK_DEPTH_CM / 100.0))
    ax2.legend(loc='lower left', framealpha=0.92, fontsize=7.8)

    # -------------------------------------------------------------------------
    # Panel 3: Pressure Derivatives dP/dt & Monotonic Volume
    # -------------------------------------------------------------------------
    ax3.plot(t, dp, color='#7c3aed', linewidth=1.5, label='dP/dt (Pressure Rate of Change)', zorder=4)
    ax3.axhline(0, color='#94a3b8', linestyle='-', linewidth=0.8)
    ax3.axhline(-2.2, color='#ef4444', linestyle=':', linewidth=1.3, label='Valve Open Trigger (-2.2 kPa/s)')
    ax3.axhline(+1.8, color='#10b981', linestyle=':', linewidth=1.3, label='Valve Closure Trigger (+1.8 kPa/s)')

    ax3.set_ylabel('dP/dt (kPa/s)', color='#7c3aed', fontweight='bold')
    ax3.tick_params(axis='y', labelcolor='#7c3aed')
    ax3.grid(True, linestyle=':', alpha=0.6)
    ax3.set_xlabel('Elapsed Time (s)', fontweight='bold')
    ax3.set_title('Pressure Derivative Dynamics & Real-Time Flow Integration', pad=5)

    ax3_r = ax3.twinx()
    ax3_r.plot(t, monotonic_vol, color='#0d9488', linewidth=2.0, linestyle='-.',
               label=f'Continuous Discharged Volume: {vol_discharged:.3f} L')
    ax3_r.set_ylabel('Cumulative Volume (L)', color='#0d9488', fontweight='bold')
    ax3_r.tick_params(axis='y', labelcolor='#0d9488')

    lines3_l, labels3_l = ax3.get_legend_handles_labels()
    lines3_r, labels3_r = ax3_r.get_legend_handles_labels()
    ax3.legend(lines3_l + lines3_r, labels3_l + labels3_r, loc='upper left', framealpha=0.92, fontsize=7.6)

    fig.suptitle(f"HIGH-RESOLUTION HYDRAULIC TRANSIENT ANALYSIS | EVENT {idx:02d}: {ename}\n"
                 f"Dual-Node Hydrodynamic Calibration Benchmark | 10 Hz Telemetry (100 ms Sample Rate)",
                 fontsize=12.0, y=0.990)

    plt.savefig(out_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Saved: {out_path}")

# Run test on Event 6 and Event 10
g6 = df[df['Event ID'] == 'EVT_2470'].copy().reset_index(drop=True)
render_event_figure(6, 'EVT_2470', 'Both Taps OPEN (m)', g6, os.path.join(OUT_DIR, 'test_refine_06.png'))

g10 = df[df['Event ID'] == 'EVT_3598'].copy().reset_index(drop=True)
render_event_figure(10, 'EVT_3598', 'All Branches open (20s)', g10, os.path.join(OUT_DIR, 'test_refine_10.png'))
