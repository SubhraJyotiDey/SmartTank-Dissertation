import matplotlib.pyplot as plt
import matplotlib.patches as patches
from matplotlib.patches import FancyBboxPatch, ArrowStyle
import numpy as np

# Set publication style
plt.rcParams['font.family'] = 'sans-serif'
plt.rcParams['font.sans-serif'] = ['DejaVu Sans', 'Arial', 'Helvetica']
plt.rcParams['font.size'] = 10

fig, ax = plt.subplots(figsize=(12, 7.5), dpi=300)
ax.set_xlim(0, 100)
ax.set_ylim(0, 100)
ax.axis('off')

# Color Palette (Academic / Industrial Clean)
c_tank = '#B0BEC5'
c_water = '#4FC3F7'
c_water_dark = '#0288D1'
c_pipe = '#78909C'
c_pump = '#FFA726'
c_sensor = '#E91E63'
c_box = '#ECEFF1'
c_edge = '#37474F'
c_node1 = '#E8F5E9'
c_node2 = '#E1F5FE'

# -------------------------------------------------------------
# 1. OVERHEAD TANK (TOP LEFT)
# -------------------------------------------------------------
tank_x, tank_y, tank_w, tank_h = 10, 60, 26, 32
water_h = 18  # ~56% full

# Tank Outer Shell
tank_rect = patches.FancyBboxPatch((tank_x, tank_y), tank_w, tank_h,
                                   boxstyle="round,pad=0.3,rounding_size=1.5",
                                   linewidth=2.5, edgecolor=c_edge, facecolor='#ECEFF1')
ax.add_patch(tank_rect)

# Water Fill
water_rect = patches.Rectangle((tank_x + 0.6, tank_y + 0.6), tank_w - 1.2, water_h,
                               facecolor=c_water, alpha=0.75, edgecolor='none')
ax.add_patch(water_rect)

# Water Surface wave indication
x_wave = np.linspace(tank_x + 0.6, tank_x + tank_w - 0.6, 100)
y_wave = tank_y + water_h + 0.3 * np.sin(np.linspace(0, 4*np.pi, 100))
ax.plot(x_wave, y_wave, color=c_water_dark, lw=1.8)

# Tank Dimensions & Labels
ax.text(tank_x + tank_w/2, tank_y + tank_h + 2.5, "1000 L Overhead Storage Tank", 
        ha='center', va='bottom', fontsize=11, fontweight='bold', color=c_edge)
ax.text(tank_x + tank_w/2, tank_y + water_h/2, "Water Column h(t)\n(122 cm Depth Span / 11.96 kPa)", 
        ha='center', va='center', fontsize=9.5, fontweight='bold', color='#01579B')

# Tank Height Dimension Line
ax.annotate('', xy=(tank_x - 3, tank_y), xytext=(tank_x - 3, tank_y + tank_h),
            arrowprops=dict(arrowstyle='<->', color=c_edge, lw=1.2))
ax.text(tank_x - 4, tank_y + tank_h/2, "122 cm\nActive Depth", 
        ha='right', va='center', fontsize=8.5, color=c_edge)

# -------------------------------------------------------------
# 2. NODE 2: ROOFTOP GROUND TRUTH SENSOR
# -------------------------------------------------------------
node2_box = patches.FancyBboxPatch((tank_x + 4, tank_y + tank_h + 1), 18, 5.5,
                                  boxstyle="round,pad=0.2,rounding_size=0.8",
                                  linewidth=1.8, edgecolor='#0288D1', facecolor=c_node2)
ax.add_patch(node2_box)
ax.text(tank_x + 13, tank_y + tank_h + 3.8, "Node 2: Overhead Ground Truth", 
        ha='center', va='center', fontsize=9, fontweight='bold', color='#01579B')
ax.text(tank_x + 13, tank_y + tank_h + 2.1, "Ultrasonic ToF (JSN-SR04T) + ESP32", 
        ha='center', va='center', fontsize=7.5, color='#0277BD')

# Ultrasonic Cone
cone_pts = [[tank_x + 13, tank_y + tank_h + 1], 
            [tank_x + 9, tank_y + water_h + 0.5], 
            [tank_x + 17, tank_y + water_h + 0.5]]
poly_cone = patches.Polygon(cone_pts, closed=True, facecolor='#29B6F6', alpha=0.25, edgecolor='#0288D1', ls='--')
ax.add_patch(poly_cone)
ax.text(tank_x + 13, tank_y + water_h + 5, "Acoustic Air Gap (d_air)\nOffset = 6.0 cm", 
        ha='center', va='center', fontsize=7.5, color='#01579B', style='italic')

# -------------------------------------------------------------
# 3. PIPING NETWORK
# -------------------------------------------------------------
# Downcomer pipe (main gravity outlet)
downcomer_x = tank_x + tank_w - 4
ax.plot([downcomer_x, downcomer_x], [tank_y, 16], color=c_pipe, lw=6, solid_capstyle='round')
# Pipe elbow to tap node
ax.plot([downcomer_x, 56], [16, 16], color=c_pipe, lw=6, solid_capstyle='round')

# Dedicated Rising Main (Pump refill pipe)
riser_x = tank_x + 3
ax.plot([riser_x, riser_x], [tank_y + tank_h - 2, 8], color='#81C784', lw=4.5, ls='-', solid_capstyle='round')
ax.plot([riser_x, 26], [8, 8], color='#81C784', lw=4.5, ls='-', solid_capstyle='round')
ax.text(riser_x - 1, 35, "Dedicated Rising Main (Refill Line)", 
        ha='center', va='center', fontsize=8, color='#2E7D32', rotation=90, fontweight='bold')

# Elevation Head Label
ax.annotate('', xy=(downcomer_x + 4, tank_y), xytext=(downcomer_x + 4, 16),
            arrowprops=dict(arrowstyle='<->', color='#D84315', lw=1.5))
ax.text(downcomer_x + 5.5, (tank_y + 16)/2, "Static Elevation Head (H_elev)\n~3.6 m (35.2 kPa Empty Baseline)", 
        ha='left', va='center', fontsize=9, fontweight='bold', color='#D84315')

# -------------------------------------------------------------
# 4. PUMP & AUTOMATION (BOTTOM LEFT)
# -------------------------------------------------------------
pump_box = patches.FancyBboxPatch((16, 4), 16, 8, boxstyle="round,pad=0.2,rounding_size=1",
                                 linewidth=1.8, edgecolor='#E65100', facecolor='#FFF3E0')
ax.add_patch(pump_box)
ax.text(24, 9.5, "AC Utility Pump", ha='center', va='center', fontsize=9, fontweight='bold', color='#E65100')
ax.text(24, 6.2, "Relay Controlled (GPIO 16)\nMax 2 Cycles/Day (Full Latch)", 
        ha='center', va='center', fontsize=7.5, color='#BF360C')

# Check Valve
ax.plot([riser_x, riser_x], [20, 24], color='#388E3C', lw=8)
ax.text(riser_x - 2, 22, "Check Valve", ha='right', va='center', fontsize=7.5, color='#2E7D32')

# -------------------------------------------------------------
# 5. NODE 1: INLINE HYDROSTATIC TAP NODE (RIGHT)
# -------------------------------------------------------------
node1_outer = patches.FancyBboxPatch((55, 10), 42, 82, boxstyle="round,pad=0.5,rounding_size=2",
                                    linewidth=2.5, edgecolor='#2E7D32', facecolor=c_node1)
ax.add_patch(node1_outer)
ax.text(76, 89, "Node 1: Non-Invasive Inline Tap Node", 
        ha='center', va='center', fontsize=12, fontweight='bold', color='#1B5E20')
ax.text(76, 86.2, "(Retrofitted at Ground Utility Outlet - No Tank Modifications)", 
        ha='center', va='center', fontsize=8.5, style='italic', color='#388E3C')

# Tap & Transducer Tee
tap_tee_x = 56
tap_tee_y = 16
# Tap Valve Symbol
ax.scatter([tap_tee_x], [tap_tee_y], s=120, color='#D32F2F', zorder=5)
ax.text(tap_tee_x - 1, tap_tee_y + 4, "Service Tap / Valve\n(Downstream Discharge)", 
        ha='center', va='bottom', fontsize=8, color='#C62828', fontweight='bold')
# Discharge arrow
ax.annotate('', xy=(tap_tee_x, tap_tee_y - 6), xytext=(tap_tee_x, tap_tee_y),
            arrowprops=dict(arrowstyle='->', color='#1976D2', lw=2.5))
ax.text(tap_tee_x + 1.5, tap_tee_y - 4, "Water Flow Q(t)", fontsize=7.5, color='#0D47A1')

# Inline Pressure Transducer Box
transducer_box = patches.FancyBboxPatch((58, 23), 36, 11, boxstyle="round,pad=0.2,rounding_size=1",
                                       linewidth=1.8, edgecolor='#C2185B', facecolor='#FCE4EC')
ax.add_patch(transducer_box)
ax.text(76, 30.5, "Piezoresistive Pressure Transducer", ha='center', va='center', 
        fontsize=9.5, fontweight='bold', color='#880E4F')
ax.text(76, 26.5, "Span: 0 to 200 kPa (0.5 - 4.5 V Analog Output)\nT-Junction Direct Hydraulic Coupling", 
        ha='center', va='center', fontsize=8, color='#AD1457')

# Coupling Line from Tee to Transducer
ax.plot([tap_tee_x, 62], [tap_tee_y, 23], color='#C2185B', lw=2.5, ls=':')

# ADS1115 External ADC Box
adc_box = patches.FancyBboxPatch((58, 38), 36, 11, boxstyle="round,pad=0.2,rounding_size=1",
                                 linewidth=1.8, edgecolor='#7B1FA2', facecolor='#F3E5F5')
ax.add_patch(adc_box)
ax.text(76, 45.5, "16-Bit Delta-Sigma External ADC (ADS1115)", ha='center', va='center', 
        fontsize=9.5, fontweight='bold', color='#4A148C')
ax.text(76, 41.5, "1,277 Conversion Steps Across 11.96 kPa Tank Span\nResolution: 0.96 mm Depth / 0.78 L Volume", 
        ha='center', va='center', fontsize=8, color='#6A1B9A')

# Wire from Sensor to ADC
ax.annotate('', xy=(76, 38), xytext=(76, 34),
            arrowprops=dict(arrowstyle='->', color='#4A148C', lw=2))
ax.text(77.5, 36, "0.5 - 4.5 V Analog (Direct 5V VIN)", fontsize=7.5, color='#4A148C')

# ESP32 Edge Processor Box
esp_box = patches.FancyBboxPatch((58, 53), 36, 20, boxstyle="round,pad=0.2,rounding_size=1",
                                 linewidth=2, edgecolor='#1565C0', facecolor='#E3F2FD')
ax.add_patch(esp_box)
ax.text(76, 70, "ESP32 Dual-Core Edge Controller", ha='center', va='center', 
        fontsize=10.5, fontweight='bold', color='#0D47A1')

# ESP32 Inner Task Boxes
fsm_subbox = patches.FancyBboxPatch((60, 55), 15.5, 12, boxstyle="round,pad=0.2,rounding_size=0.8",
                                    linewidth=1.2, edgecolor='#0D47A1', facecolor='#BBDEFB')
ax.add_patch(fsm_subbox)
ax.text(67.75, 64, "Core 1: Deterministic FSM", ha='center', va='center', fontsize=8, fontweight='bold', color='#0D47A1')
ax.text(67.75, 59.5, "- STATIC (Lockout)\n- TAP_OPEN (dP/dt)\n- SETTLING (Hammer)", 
        ha='center', va='center', fontsize=7, color='#1565C0')

task_subbox = patches.FancyBboxPatch((77, 55), 15.5, 12, boxstyle="round,pad=0.2,rounding_size=0.8",
                                     linewidth=1.2, edgecolor='#0D47A1', facecolor='#BBDEFB')
ax.add_patch(task_subbox)
ax.text(84.75, 64, "Core 0: FreeRTOS Task", ha='center', va='center', fontsize=8, fontweight='bold', color='#0D47A1')
ax.text(84.75, 59.5, "- 100 ms Telemetry Log\n- HTTP/MQTT Pipeline\n- Rooftop Poller Sync", 
        ha='center', va='center', fontsize=7, color='#1565C0')

# Wire from ADC to ESP32 (I2C)
ax.annotate('', xy=(76, 53), xytext=(76, 49),
            arrowprops=dict(arrowstyle='<->', color='#1565C0', lw=2))
ax.text(77.5, 51, "Hardware I2C Bus (SDA / SCL)", fontsize=7.5, color='#1565C0')

# Relay Control Wire from ESP32 to Pump
ax.annotate('', xy=(32, 8), xytext=(58, 58),
            arrowprops=dict(arrowstyle='->', color='#E65100', lw=1.8, ls='--',
                            connectionstyle="arc3,rad=0.25"))
ax.text(45, 30, "Pump Relay Drive (GPIO 16)\nLatch-Till-Full Logic", 
        ha='center', va='center', fontsize=7.5, color='#BF360C', fontweight='bold',
        bbox=dict(boxstyle="round,pad=0.2", fc="#FFF3E0", ec="#FFA726", lw=1))

# Cloud / Dashboard Telemetry (Top Right)
cloud_box = patches.FancyBboxPatch((60, 76), 32, 6, boxstyle="round,pad=0.2,rounding_size=0.8",
                                  linewidth=1.2, edgecolor='#00695C', facecolor='#E0F2F1')
ax.add_patch(cloud_box)
ax.text(76, 79, "Wi-Fi Telemetry: REST API / MQTT / Web Dashboard", 
        ha='center', va='center', fontsize=8, fontweight='bold', color='#004D40')

# Rooftop Sync Arrow (Wireless between Node 2 and Node 1)
ax.annotate('', xy=(60, 80), xytext=(tank_x + 22, tank_y + tank_h + 3.8),
            arrowprops=dict(arrowstyle='<->', color='#0288D1', lw=1.8, ls=':',
                            connectionstyle="arc3,rad=-0.15"))
ax.text(42, 88, "HTTP Validation Sync (1 Hz)\nGround Truth Comparison", 
        ha='center', va='center', fontsize=8, color='#0277BD', fontweight='bold')

plt.tight_layout()
fig.savefig('A:/Dissertation/code/figures/system_schematic.png', dpi=300, bbox_inches='tight')
fig.savefig('A:/Dissertation/code/figures/system_schematic.pdf', bbox_inches='tight')
print("Successfully generated system_schematic.png and system_schematic.pdf")
