#!/usr/bin/env python3
"""issue1_views.py [out.env]: fixed views in 03_river for the Issue #1 picture check (docs/phase1/issue-1-grafik.md 11),
the same on the Steam Deck (tools/deck/issue1.sh VIEWS=...) and the PC (tools/issue1_pc.ps1), so that the frame dumps
of both can be paired by level time.

Each spot: teleport at T, then 4 views 90 degrees apart (mouse 900 counts = 90 degrees at the default 0.1 degree per
count). View v is held from T + v*STEP + 0.2 to T + v*STEP + HOLD; the turn to the next view takes TURN seconds. During
the hold the teleport is repeated every 0.5 s (the player stays where he is, and every teleport logs level and wall time,
which pairs the frame dumps with the views). Writes FCP_TEST_TELEPORT, FCP_KEY_SCRIPT and ISSUE1_VIEWS (name and level
time window per view, for issue1_pick.py)."""
import sys

SPOTS = [  # name, x, y, z, pitch in mouse counts (my, + = down; 10 counts = 1 degree), first spot sets it
    ("crates_out", 364.5, 525.0, 22.5, 0),
    ("crates_n", 361.0, 526.6, 22.6, 350),
    ("crates_s", 361.0, 523.0, 22.6, 350),
    ("shore_w", 322.0, 625.0, 28.5, 0),
    ("shore_e", 360.0, 618.0, 28.5, 0),
    ("water", 338.0, 580.0, 18.6, 0),
    ("jetty", 353.8, 553.2, 22.5, 0),
    ("bridge", 340.0, 631.5, 28.5, 0),
]
T0, HOLD, TURN = 12.0, 3.0, 0.4
STEP = HOLD + TURN
tp, ks, views = [], [], []
t = T0
pitch = 0
for name, x, y, z, want in SPOTS:
    if want != pitch:  # in the gap before the spot's first view (the hold window starts at +1.0)
        ks.append(f"{t + 0.1:.1f}-{t + 0.5:.1f}:my={want - pitch}")
        pitch = want
    for v in range(4):
        a = t + v * STEP
        k = a
        while k < a + HOLD - 0.01:
            tp.append(f"{k:.1f}:{x},{y},{z}")
            k += 0.5
        views.append(f"{name}_{v * 90}:{a + 1.0:.1f}-{a + HOLD:.1f}")
        ks.append(f"{a + HOLD:.1f}-{a + HOLD + TURN - 0.05:.2f}:mx=900")
    t += 4 * STEP + 0.6
lines = [f"FCP_TEST_TELEPORT={'|'.join(tp)}", f"FCP_KEY_SCRIPT={','.join(ks)}", f"ISSUE1_VIEWS={','.join(views)}",
         f"ISSUE1_END={t:.0f}"]
out = open(sys.argv[1], "w", newline="\n") if len(sys.argv) > 1 else sys.stdout
out.write("\n".join(lines) + "\n")
