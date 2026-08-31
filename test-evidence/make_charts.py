import os
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker

CSV = os.path.join(os.path.dirname(__file__), "four_led_test_results_2026-08-31.csv")
OUT_DIR = os.path.join(os.path.dirname(__file__), "charts")

df = pd.read_csv(CSV)

INK = "#1a1a1a"
GRID = "#dddddd"
BLUE = "#2f6fed"
ORANGE = "#e0762a"
RED = "#c0392b"
GREY = "#8a8a8a"

plt.rcParams.update({
    "font.size": 11,
    "text.color": INK,
    "axes.edgecolor": GRID,
    "axes.labelcolor": INK,
    "xtick.color": INK,
    "ytick.color": INK,
    "axes.grid": True,
    "grid.color": GRID,
    "grid.linewidth": 0.8,
    "figure.facecolor": "white",
    "axes.facecolor": "white",
    "savefig.facecolor": "white",
})


def style_axes(ax):
    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.spines["left"].set_color(GRID)
    ax.spines["bottom"].set_color(GRID)
    ax.set_axisbelow(True)


# --- Chart 1: staged discharge, SoC vs power allowance/usage --------------
d1 = df[df["section"].str.startswith("Staged discharge")].copy()
d1["soc_percent"] = d1["soc_percent"].astype(float)
d1 = d1.sort_values("soc_percent", ascending=False)

fig, ax = plt.subplots(figsize=(7.5, 4.6), dpi=200)
x = d1["soc_percent"].values
ax.plot(x, d1["P_auto_available_W"], marker="o", color=BLUE, linewidth=2.2, label="P_auto_available (W)")
ax.plot(x, d1["P_auto_W"], marker="o", color=ORANGE, linewidth=2.2, label="P_auto selected (W)")
ax.axhline(60, color=GREY, linestyle="--", linewidth=1.2)
ax.text(22, 64, "60 W = one AUTO load", color=GREY, fontsize=9, ha="left")
ax.axhline(20, color=RED, linestyle=":", linewidth=1.2)
ax.text(22, 24, "P_reserve = 20 W floor", color=RED, fontsize=9, ha="left")

for xi, yi in zip(x, d1["P_auto_W"]):
    ax.annotate(f"{yi:.0f} W", (xi, yi), textcoords="offset points", xytext=(0, -16),
                ha="center", fontsize=9, color=ORANGE)

ax.set_xlabel("Battery SoC (%)")
ax.set_ylabel("Power (W)")
ax.set_ylim(-5, 140)
ax.set_title("Staged discharge: AUTO power allowance shrinks as SoC falls\n(300 Ah, 15 V, 4 h runtime target, GPIO 27 fixed at 60 W)")
ax.invert_xaxis()
ax.xaxis.set_major_locator(mticker.FixedLocator(sorted(x, reverse=True)))
ax.legend(frameon=False, loc="lower center", bbox_to_anchor=(0.5, -0.28), ncol=2)
style_axes(ax)
fig.tight_layout()
fig.savefig(f"{OUT_DIR}/discharge_soc_vs_power.png", bbox_inches="tight")
plt.close(fig)


# --- Chart 2: runtime target vs power allowance ----------------------------
d2 = df[df["section"].str.startswith("Runtime comparison")].copy()
d2["runtime_target_h"] = d2["runtime_target_h"].astype(float)
d2 = d2.sort_values("runtime_target_h")

fig, ax = plt.subplots(figsize=(7.5, 4.6), dpi=200)
x = d2["runtime_target_h"].values
ax.plot(x, d2["P_auto_available_W"], marker="o", color=BLUE, linewidth=2.2, label="P_auto_available (W)")
ax.plot(x, d2["P_auto_W"], marker="o", color=ORANGE, linewidth=2.2, label="P_auto selected (W)")
ax.axhline(60, color=GREY, linestyle="--", linewidth=1.2)
ax.text(x.min(), 63, "60 W = one AUTO load", color=GREY, fontsize=9)

for xi, yi in zip(x, d2["P_auto_available_W"]):
    ax.annotate(f"{yi:.1f} W", (xi, yi), textcoords="offset points", xytext=(0, 10),
                ha="center", fontsize=9, color=BLUE)

ax.set_xlabel("Runtime target (hours)")
ax.set_ylabel("Power (W)")
ax.set_title("Longer runtime targets reduce sustainable AUTO power\n(300 Ah, 15 V, 35% SoC, GPIO 27 fixed at 60 W)")
ax.set_xticks(x)
ax.legend(frameon=False, loc="upper right")
style_axes(ax)
fig.tight_layout()
fig.savefig(f"{OUT_DIR}/runtime_target_vs_power.png")
plt.close(fig)


# --- Chart 3: battery capacity vs power allowance --------------------------
d3 = df[df["section"].str.startswith("Capacity comparison")].copy()
d3["battery_Ah"] = d3["battery_Ah"].astype(float)
d3 = d3.sort_values("battery_Ah")

fig, ax = plt.subplots(figsize=(7.5, 4.6), dpi=200)
x = d3["battery_Ah"].values
ax.plot(x, d3["P_auto_available_W"], marker="o", color=BLUE, linewidth=2.2, label="P_auto_available (W)")
ax.plot(x, d3["P_auto_W"], marker="o", color=ORANGE, linewidth=2.2, label="P_auto selected (W)")
ax.axhline(120, color=GREY, linestyle="--", linewidth=1.2)
ax.text(150, 123, "120 W = P_budget - P_reserve - P_fixed ceiling", color=GREY, fontsize=9)

for xi, yi in zip(x, d3["P_auto_available_W"]):
    ax.annotate(f"{yi:.1f} W", (xi, yi), textcoords="offset points", xytext=(0, 10),
                ha="center", fontsize=9, color=BLUE)

ax.set_xlabel("Battery capacity (Ah)")
ax.set_ylabel("Power (W)")
ax.set_ylim(-5, 140)
ax.set_title("More battery capacity raises AUTO allowance, then hits the budget ceiling\n(15 V, 35% SoC, 4 h runtime target, GPIO 27 fixed at 60 W)")
ax.set_xticks(x)
ax.legend(frameon=False, loc="lower center", bbox_to_anchor=(0.5, -0.28), ncol=2)
style_axes(ax)
fig.tight_layout()
fig.savefig(f"{OUT_DIR}/capacity_vs_power.png", bbox_inches="tight")
plt.close(fig)


# --- Chart 4: GPIO state timeline across all recorded scenarios ------------
gpios = ["gpio4_state", "gpio5_state", "gpio25_state", "gpio27_state"]
labels = ["GPIO 4", "GPIO 5", "GPIO 25", "GPIO 27"]

def color_for(state):
    if state in ("AUTO_ON", "FIXED_ON"):
        return BLUE if state == "AUTO_ON" else RED
    return "#f0f0f0"

fig, ax = plt.subplots(figsize=(11, 4.2), dpi=200)
for row_i, (_, row) in enumerate(df.iterrows()):
    for col_i, g in enumerate(gpios):
        ax.add_patch(plt.Rectangle((row_i, col_i), 1, 1, facecolor=color_for(row[g]),
                                    edgecolor="white", linewidth=1.5))

ax.set_xlim(0, len(df))
ax.set_ylim(0, len(gpios))
ax.set_xticks([i + 0.5 for i in range(len(df))])
ax.set_xticklabels(df["scenario_id"], fontsize=8)
ax.set_yticks([i + 0.5 for i in range(len(gpios))])
ax.set_yticklabels(labels)
ax.set_xlabel("Scenario ID (see CSV / report)")
ax.set_title(f"GPIO state across all {len(df)} recorded scenarios")
ax.grid(False)
for spine in ax.spines.values():
    spine.set_visible(False)
ax.tick_params(length=0)

from matplotlib.patches import Patch
legend_elems = [
    Patch(facecolor=BLUE, label="AUTO_ON"),
    Patch(facecolor=RED, label="FIXED_ON"),
    Patch(facecolor="#f0f0f0", edgecolor=GRID, label="OFF (AUTO_OFF / FIXED_OFF)"),
]
ax.legend(handles=legend_elems, frameon=False, loc="upper center",
          bbox_to_anchor=(0.5, -0.18), ncol=3)
fig.tight_layout()
fig.savefig(f"{OUT_DIR}/gpio_state_timeline.png", bbox_inches="tight")
plt.close(fig)

print("done")
