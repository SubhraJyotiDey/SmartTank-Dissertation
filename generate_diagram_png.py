import matplotlib.pyplot as plt
import matplotlib.patches as patches

# Set up figure with high resolution
fig, ax = plt.subplots(figsize=(18, 7.5), dpi=300)
fig.patch.set_facecolor('#FFFFFF')
ax.set_facecolor('#FFFFFF')

# Blocks layout: (x, y, width, height, title, subtitle, fill_color, border_color)
w = 2.25
h = 2.5
y = 2.5
pad = 0.06

blocks = [
    {
        "x": 0.6, "y": y, "w": w, "h": h,
        "title": "Overhead Tank\n(Water Source)",
        "sub": "• Rooftop Tank\n• Variable Head $H_{level}$\n• Non-invasive baseline",
        "fill": "#EBF8FF", "border": "#0284C7"
    },
    {
        "x": 4.15, "y": y, "w": w, "h": h,
        "title": "Inline Sensor\n(at Tap Point)",
        "sub": "• Pressure Transducer\n  (Piezoresistive)\n       -- OR --\n• Magnetic Hall Sensor",
        "fill": "#FFFBEB", "border": "#D97706"
    },
    {
        "x": 7.7, "y": y, "w": w, "h": h,
        "title": "Signal Conditioning\n& ADC",
        "sub": "• Anti-aliasing LPF\n• Surge/Spike Protection\n• 16-bit Precision ADC",
        "fill": "#FAF5FF", "border": "#9333EA"
    },
    {
        "x": 11.25, "y": y, "w": w, "h": h,
        "title": "ESP32 Controller\n& Web Server",
        "sub": "• Pressure Drop Trigger\n• Hydrostatic Deduction\n• Local Web Server",
        "fill": "#F0FDF4", "border": "#16A34A"
    },
    {
        "x": 14.8, "y": y, "w": w, "h": h,
        "title": "Web Dashboard\n(Mobile / PC)",
        "sub": "• Real-time Tank %\n• Volume in Liters\n• Calibration Controls",
        "fill": "#FFF1F2", "border": "#E11D48"
    }
]

# Draw blocks
for b in blocks:
    bx, by, bw, bh = b["x"], b["y"], b["w"], b["h"]
    
    # Shadow
    shadow = patches.FancyBboxPatch(
        (bx + 0.04, by - 0.05), bw, bh,
        boxstyle=f"round,pad={pad},rounding_size=0.15",
        facecolor="#E2E8F0",
        edgecolor="none",
        alpha=0.7,
        zorder=2
    )
    ax.add_patch(shadow)

    # Main Card
    rect = patches.FancyBboxPatch(
        (bx, by), bw, bh,
        boxstyle=f"round,pad={pad},rounding_size=0.15",
        facecolor=b["fill"],
        edgecolor=b["border"],
        linewidth=2.2,
        zorder=3
    )
    ax.add_patch(rect)
    
    # Header Banner
    banner_h = 0.8
    header = patches.FancyBboxPatch(
        (bx, by + bh - banner_h), bw, banner_h,
        boxstyle=f"round,pad={pad},rounding_size=0.15",
        facecolor=b["border"],
        edgecolor=b["border"],
        linewidth=1,
        zorder=4
    )
    ax.add_patch(header)
    
    # Title Text
    ax.text(
        bx + bw/2, by + bh - banner_h/2,
        b["title"],
        ha='center', va='center',
        fontsize=11.5, fontweight='bold',
        color='#FFFFFF', zorder=5
    )
    
    # Subtitle / Body text
    ax.text(
        bx + 0.18, by + (bh - banner_h)/2 + 0.05,
        b["sub"],
        ha='left', va='center',
        fontsize=9.5, color='#1E293B',
        linespacing=1.45,
        zorder=5
    )

# Connective arrows
arrow_y = 3.65
arrows_data = [
    (blocks[0]["x"] + w + pad + 0.05, blocks[1]["x"] - pad - 0.05, "Gravity\nWater Flow"),
    (blocks[1]["x"] + w + pad + 0.05, blocks[2]["x"] - pad - 0.05, "Raw Sensor\nSignal"),
    (blocks[2]["x"] + w + pad + 0.05, blocks[3]["x"] - pad - 0.05, "Conditioned\nDigital Data"),
    (blocks[3]["x"] + w + pad + 0.05, blocks[4]["x"] - pad - 0.05, "Wi-Fi (HTTP /\nWebSocket)")
]

for x_start, x_end, label in arrows_data:
    ax.annotate(
        "",
        xy=(x_end, arrow_y), xycoords='data',
        xytext=(x_start, arrow_y), textcoords='data',
        arrowprops=dict(
            arrowstyle="-|>",
            color="#334155",
            lw=2.5,
            mutation_scale=18
        ),
        zorder=4
    )
    mid_x = (x_start + x_end) / 2
    ax.text(
        mid_x, arrow_y + 0.18,
        label,
        ha='center', va='bottom',
        fontsize=8.5, fontweight='bold',
        color="#334155", zorder=6
    )

# Diagram Title & Subtitle
plt.text(
    8.8, 6.15,
    "Non-Invasive Inline, Pressure Drop Activated Tank Water Level Estimator",
    ha='center', va='center',
    fontsize=16.5, fontweight='bold', color='#0F172A'
)
plt.text(
    8.8, 5.7,
    "System Architecture Block Diagram",
    ha='center', va='center',
    fontsize=13, fontweight='semibold', color='#475569'
)

# Axis Configuration
ax.set_xlim(0, 17.8)
ax.set_ylim(1.8, 6.7)
ax.axis('off')

# Save outputs
plt.tight_layout()
output_path = r"c:\Users\subhr\Desktop\Dissertation\code\system_block_diagram.png"
artifact_path = r"C:\Users\subhr\.gemini\antigravity\brain\fc2ac66f-c0ef-4b73-b95e-1f788961dee8\system_block_diagram.png"

plt.savefig(output_path, dpi=300, bbox_inches='tight')
plt.savefig(artifact_path, dpi=300, bbox_inches='tight')
print("Crystal clear PNG generated!")
