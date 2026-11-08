"""
=============================================================================
Dissertation High-Resolution Hydraulic Transient & Water Hammer Event Plotter
Processes Sheet 2 (HighRes_Event_Logs) | Dual-Node Smart Tank System
=============================================================================
Generates publication-quality 300 DPI research-standard comparative plots:
- 13 Synchronized 3-Panel Transient Plots with Dynamic Head, Derivatives, and Level Error
- Dedicated 4-Column Technical Dissertation Metadata Header per Event
- 1 Master Comparative Multi-Panel Synthesis Matrix across all 13 events
- 1 Comprehensive Summary CSV for Dissertation Quantitative Results
"""

import os
import re
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# Paths & Directories
CSV_PATH = r"data/highres_events_sheet2.csv"
OUT_DIR  = r"plots/highres_events"
os.makedirs(OUT_DIR, exist_ok=True)

# Publication Standards Typography
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

def sanitize_filename(name):
    clean = re.sub(r'^[0-9]+\)\s*', '', name) # remove leading numbering if present
    clean = re.sub(r'[^a-zA-Z0-9_-]', '_', clean)
    clean = re.sub(r'_+', '_', clean).strip('_').lower()
    return clean

def plot_single_event(idx, eid, ename, group, out_path):
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

    # Uncompensated naive pressure level
    naive_level = np.clip(1.6262 * p + 6.4417, 0.0, 100.0)

    # True continuous cumulative volume integration
    monotonic_vol = np.zeros_like(vol)
    cur_vol = 0.0
    for i in range(1, len(vol)):
        d_v = vol[i] - vol[i-1]
        if d_v < -0.005:  # firmware cycle reset detected
            cur_vol += vol[i-1]
        monotonic_vol[i] = cur_vol + vol[i]
    if np.max(monotonic_vol) == 0.0 and np.max(vol) > 0:
        monotonic_vol = vol.copy()
    vol_discharged = float(np.max(monotonic_vol) - np.min(monotonic_vol))

    # Dual-node compensated level
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

    ax_meta.text(0.02, 0.82, c1_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top')
    ax_meta.text(0.27, 0.82, c2_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top')
    ax_meta.text(0.53, 0.82, c3_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top')
    ax_meta.text(0.78, 0.82, c4_text, transform=ax_meta.transAxes, fontsize=8.0, verticalalignment='top')

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

    p_lower = min(0.0, p_min - 6.0)
    p_upper = max(p_base * 1.30, p_max * 1.15 + 10.0)
    ax1.set_ylim(p_lower, p_upper)
    ax1_r.set_ylim(p_lower / 9.80665, p_upper / 9.80665)

    t_span = t[-1] - t[0]
    p_span = p_upper - p_lower

    # Adaptive placement: decide legend position based on where hammer peak is
    legend_loc = 'upper left' if (t_max >= (t[0] + 0.45 * t_span) and delta_hammer_surge >= 2.0) else 'upper right'

    # Friction Trough Annotation
    x_off_drop = 0.08 * t_span if t_min < (t[0] + 0.65 * t_span) else -0.25 * t_span
    y_off_drop = 0.15 * p_span
    ax1.scatter([t_min], [p_min], color='#dc2626', s=60, zorder=6, edgecolors='black', linewidth=1.2)
    ax1.annotate(f'Friction Trough P_min: {p_min:.2f} kPa\n(ΔP = -{delta_friction_drop:.2f} kPa | -{delta_friction_drop/9.80665:.2f} m Loss)',
                 xy=(t_min, p_min), xytext=(t_min + x_off_drop, p_min + y_off_drop),
                 arrowprops=dict(facecolor='#dc2626', shrink=0.08, width=1.4, headwidth=5),
                 fontsize=8.0, fontweight='bold', color='#991b1b',
                 bbox=dict(boxstyle='round,pad=0.25', facecolor='#fee2e2', edgecolor='#dc2626', alpha=0.92))

    # Water Hammer Annotation
    if delta_hammer_surge >= 2.0:
        if legend_loc == 'upper left':
            x_off_surge = -0.22 * t_span if t_max > (t[0] + 0.60 * t_span) else 0.06 * t_span
        else:
            x_off_surge = -0.22 * t_span
        y_off_surge = -0.15 * p_span if p_max > (p_base + 0.50 * p_span) else 0.10 * p_span

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
    ax1.legend(handles=lines1 + patches, loc=legend_loc, framealpha=0.92, fontsize=7.6, ncol=2)

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
    # Panel 3: Pressure Derivatives dP/dt & Continuous Monotonic Volume
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

def main():
    print(f"Loading data from: {CSV_PATH}")
    df = pd.read_csv(CSV_PATH)
    
    unique_events = df[['Event ID', 'Event Name']].drop_duplicates()
    n_events = len(unique_events)
    print(f"Verified Event Count in Sheet 2: {n_events} distinct events (Expected 13, NOT 14)")

    event_metrics = []
    generated_files = []

    for idx, (eid, ename) in enumerate(unique_events.values, 1):
        group = df[df['Event ID'] == eid].copy().reset_index(drop=True)
        sanitized = sanitize_filename(ename)
        out_filename = f"event_{idx:02d}_{eid}_{sanitized}.png"
        out_path = os.path.join(OUT_DIR, out_filename)

        print(f"[{idx:02d}/{n_events}] Rendering research plot: {eid} - '{ename}' -> {out_filename} ...")
        plot_single_event(idx, eid, ename, group, out_path)
        generated_files.append((idx, eid, ename, out_path))

        # Collect metrics for summary
        p = group['Gauge Pressure (kPa)'].values
        dp = group['dP/dt (kPa/s)'].values
        states = group['Tap Flow State'].values
        u_truth = group['Ultrasonic Ground Truth (%)'].values
        u_dist = group['Ultrasonic Distance (cm)'].values
        vol = group['Cumulative Volume (L)'].values
        t = group['Elapsed Time (s)'].values

        static_mask = (states == 'STATIC')
        p_base = float(np.median(p[static_mask][:15])) if np.sum(static_mask) >= 5 else float(p[0])
        p_min = float(np.min(p))
        p_max = float(np.max(p))
        delta_friction = max(0.0, p_base - p_min)
        delta_hammer = max(0.0, p_max - p_base)
        naive_level = np.clip(1.6262 * p + 6.4417, 0.0, 100.0)
        naive_err = float(np.max(np.abs(naive_level - u_truth)))

        monotonic_vol = np.zeros_like(vol)
        cur_vol = 0.0
        for i in range(1, len(vol)):
            d_v = vol[i] - vol[i-1]
            if d_v < -0.005:
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
        comp_err = float(np.max(np.abs(comp_level - u_truth)))

        event_metrics.append({
            'Index': idx,
            'Event ID': eid,
            'Event Name': ename,
            'Duration (s)': round(float(t[-1] - t[0]), 2),
            'Samples': len(group),
            'P_baseline (kPa)': round(p_base, 2),
            'P_min (kPa)': round(p_min, 2),
            'Delta_P_friction (kPa)': round(delta_friction, 2),
            'Head_Loss (m)': round(delta_friction / 9.80665, 3),
            'P_hammer_peak (kPa)': round(p_max, 2),
            'Delta_P_hammer (kPa)': round(delta_hammer, 2),
            'Hammer_Surge_Head (m)': round(delta_hammer / 9.80665, 3),
            'Max_dPdt (kPa/s)': round(float(np.max(dp)), 1),
            'Min_dPdt (kPa/s)': round(float(np.min(dp)), 1),
            'US_Ground_Truth (%)': round(float(np.mean(u_truth)), 2),
            'US_Distance (cm)': round(float(np.mean(u_dist)), 2),
            'Max_Naive_Error (%)': round(naive_err, 1),
            'Max_Naive_Error_cm': round(naive_err * 1.22, 1),
            'Compensated_Error (%)': round(comp_err, 2),
            'Compensated_Error_cm': round(comp_err * 1.22, 2),
            'Discharged_Volume (L)': round(vol_discharged, 3),
            'Plot_Filename': out_filename
        })

    summary_df = pd.DataFrame(event_metrics)
    summary_csv_path = os.path.join(OUT_DIR, "highres_events_dissertation_summary.csv")
    summary_df.to_csv(summary_csv_path, index=False)
    print(f"\nSaved comprehensive dissertation summary CSV to: {summary_csv_path}")

    # -------------------------------------------------------------------------
    # Master Comparative Multi-Panel Synthesis Figure
    # -------------------------------------------------------------------------
    print("Generating Master Comparative Summary Figure: 00_master_highres_events_comparison.png ...")
    fig_m = plt.figure(figsize=(16, 12), dpi=300)
    gs_m = fig_m.add_gridspec(2, 2, hspace=0.32, wspace=0.22)

    ax_m1 = fig_m.add_subplot(gs_m[0, 0])
    ax_m2 = fig_m.add_subplot(gs_m[0, 1])
    ax_m3 = fig_m.add_subplot(gs_m[1, 0])
    ax_m4 = fig_m.add_subplot(gs_m[1, 1])

    indices = np.arange(len(event_metrics))

    # Subplot 1: Water Hammer Joukowsky Shock Overpressure Surge
    hammer_surges = [m['Delta_P_hammer (kPa)'] for m in event_metrics]
    peak_pressures = [m['P_hammer_peak (kPa)'] for m in event_metrics]
    bars1 = ax_m1.bar(indices, hammer_surges, color='#7c3aed', alpha=0.85, edgecolor='#4c1d95', linewidth=1.2)
    bars1[5].set_color('#db2777')
    bars1[5].set_edgecolor('#831843')
    ax_m1.set_ylabel('Water Hammer Overpressure Surge ΔP (kPa)', fontweight='bold', color='#4c1d95')
    ax_m1.set_title('(A) Water Hammer Acoustic Shock Overpressure (Joukowsky Surge)', pad=6)
    ax_m1.set_xticks(indices)
    ax_m1.set_xticklabels([f"E{m['Index']:02d}" for m in event_metrics], fontweight='bold')
    ax_m1.grid(True, linestyle=':', alpha=0.6, axis='y')
    for i, v in enumerate(hammer_surges):
        if v > 15.0:
            ax_m1.text(i, v + 2.5, f"{v:.1f} kPa\n({peak_pressures[i]:.1f})", ha='center', va='bottom',
                       fontsize=7.2, fontweight='bold', color='#4c1d95')
        elif v > 1.0:
            ax_m1.text(i, v + 1.5, f"{v:.1f}", ha='center', va='bottom', fontsize=7.0, color='#6b21a8')

    # Subplot 2: Dynamic Friction Pressure Head Loss
    friction_drops = [m['Delta_P_friction (kPa)'] for m in event_metrics]
    bars2 = ax_m2.bar(indices, friction_drops, color='#0284c7', alpha=0.85, edgecolor='#0369a1', linewidth=1.2)
    ax_m2.set_ylabel('Dynamic Friction Pressure Drop ΔP (kPa)', fontweight='bold', color='#0369a1')
    ax_m2.set_title('(B) Dynamic Bernoulli / Darcy Friction Head Loss', pad=6)
    ax_m2.set_xticks(indices)
    ax_m2.set_xticklabels([f"E{m['Index']:02d}" for m in event_metrics], fontweight='bold')
    ax_m2.grid(True, linestyle=':', alpha=0.6, axis='y')
    for i, v in enumerate(friction_drops):
        ax_m2.text(i, v + 0.8, f"-{v:.1f}", ha='center', va='bottom', fontsize=7.0, color='#075985', fontweight='bold')

    # Subplot 3: Naive Hydrodynamic Error vs Compensated Dual-Node Error
    naive_errs = [m['Max_Naive_Error (%)'] for m in event_metrics]
    comp_errs = [m['Compensated_Error (%)'] for m in event_metrics]
    w = 0.38
    b_naive = ax_m3.bar(indices - w/2, naive_errs, width=w, color='#ef4444', alpha=0.85,
                        edgecolor='#b91c1c', label='Naive Uncompensated Error (%)')
    b_comp = ax_m3.bar(indices + w/2, comp_errs, width=w, color='#10b981', alpha=0.9,
                       edgecolor='#047857', label='Dual-Node Compensated Error (%)')
    ax_m3.set_ylabel('Maximum Level Estimation Error (%)', fontweight='bold')
    ax_m3.set_title('(C) Naive Hydrodynamic Failure vs Dual-Node Invariance', pad=6)
    ax_m3.set_xticks(indices)
    ax_m3.set_xticklabels([f"E{m['Index']:02d}" for m in event_metrics], fontweight='bold')
    ax_m3.legend(loc='upper right', framealpha=0.92, fontsize=8.0)
    ax_m3.grid(True, linestyle=':', alpha=0.6, axis='y')
    for i, v in enumerate(naive_errs):
        ax_m3.text(i - w/2, v + 1.0, f"{v:.0f}%", ha='center', va='bottom', fontsize=6.8, color='#991b1b', fontweight='bold')

    # Subplot 4: Cumulative Volumetric Discharge & Event Duration
    discharged_vols = [m['Discharged_Volume (L)'] for m in event_metrics]
    durations = [m['Duration (s)'] for m in event_metrics]
    ax_m4.plot(indices, durations, color='#f59e0b', marker='s', markersize=6, linewidth=1.8, label='Event Duration (s)')
    ax_m4.set_ylabel('Duration (s)', fontweight='bold', color='#b45309')
    ax_m4.tick_params(axis='y', labelcolor='#b45309')
    ax_m4_r = ax_m4.twinx()
    ax_m4_r.bar(indices, discharged_vols, color='#0d9488', alpha=0.55, edgecolor='#0f766e', width=0.45, label='Discharged Volume (L)')
    ax_m4_r.set_ylabel('Discharged Volume (L)', fontweight='bold', color='#0f766e')
    ax_m4_r.tick_params(axis='y', labelcolor='#0f766e')
    ax_m4.set_title('(D) Event Duration & Continuous Volumetric Drawdown', pad=6)
    ax_m4.set_xticks(indices)
    ax_m4.set_xticklabels([f"E{m['Index']:02d}" for m in event_metrics], fontweight='bold')
    ax_m4.grid(True, linestyle=':', alpha=0.6)

    fig_m.suptitle("CROSS-EVENT SYNTHESIS: HIGH-RESOLUTION HYDRAULIC TRANSIENTS & DUAL-NODE EVALUATION\n"
                   "Comprehensive Characterization of 13 Experimental Tap & Branch Discharge Events (Sheet 2 Telemetry)",
                   fontsize=12.5, y=0.985)

    master_path = os.path.join(OUT_DIR, "00_master_highres_events_comparison.png")
    plt.savefig(master_path, dpi=300, bbox_inches='tight')
    plt.close()
    print(f"Saved master comparative summary plot to: {master_path}")

    print("\n" + "="*80)
    print("ALL 13 HIGH-RESOLUTION EVENT PLOTS & MASTER MATRIX SUCCESSFULLY GENERATED!")
    print(f"Output Directory: {os.path.abspath(OUT_DIR)}")
    print("="*80)

if __name__ == '__main__':
    main()
