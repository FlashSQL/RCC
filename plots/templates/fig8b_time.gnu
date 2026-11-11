set terminal pdf size 3,2 font "Helvetica,17"
set output 'fig8b_time.pdf'
DATA = 'time_split.dat'

set ylabel "Amortized Runtime (ms)" offset 0.8,0
set xlabel "Number of Threads" offset 0,1
YMAX = 1.7
set yrange [0:YMAX]
set ytics 0.5 format "%g" font "Helvetica,17"
set grid ytics lt 1 lc rgb "#dddddd"
set xrange [0:20]
set xtics ("32" 2.5, "64" 7.5, "96" 12.5, "128" 17.5) font "Helvetica,17"
set xtics offset 0,0.5 out nomirror
set ytics out nomirror
set tics scale 0.5
set border 2+4+8
# Keep the reference plot width and leave room below for the larger text.
set lmargin at screen (39.501/215.0)
set rmargin at screen (197.151/215.0)
set tmargin at screen (132.351/143.0)
set bmargin at screen (28.0/143.0)
unset key

# Source order: BB, RCC (LUI), RCC (LUI+ODH), RCC.
# Five columns per system: Execution, Lock Wait, Commit Wait, Prevention, Wasted.
# Bottom to top: Wasted, Prevention, Commit Wait, Lock Wait, Execution.
COLS = "#a0d084 #b0b0b0 #9cb2e3 #ff7f0e"
pos(i,j) = 5*(i-2)+j
bw = 1.0

# Use 13pt text for the component and system legends.
set object 211 rect from screen 0.20,0.870 to screen 0.24,0.900 fc rgb "#ffffff" fs solid 1.0 border lc rgb "#000000" front
set label 211 "Execution" at screen 0.25,0.885 left font "Helvetica,13" tc rgb "#222222" front
set object 212 rect from screen 0.50,0.870 to screen 0.54,0.900 fc rgb "#000000" fs transparent pattern 7 border lc rgb "#000000" front
set label 212 "Lock Wait" at screen 0.55,0.885 left font "Helvetica,13" tc rgb "#222222" front
set object 213 rect from screen 0.20,0.795 to screen 0.24,0.825 fc rgb "#ffffff" fs solid 1.0 border lc rgb "#000000" front
set label 213 "Commit Wait" at screen 0.25,0.810 left font "Helvetica,13" tc rgb "#222222" front
set object 214 rect from screen 0.50,0.795 to screen 0.54,0.825 fc rgb "#ffffff" fs solid 1.0 border lc rgb "#000000" front
set label 214 "Prevention" at screen 0.55,0.810 left font "Helvetica,13" tc rgb "#222222" front
set object 215 rect from screen 0.20,0.720 to screen 0.24,0.750 fc rgb "#000000" fs transparent pattern 10 border lc rgb "#000000" front
set label 215 "Wasted" at screen 0.25,0.735 left font "Helvetica,13" tc rgb "#222222" front

# Same arrangement as the TPS graph: RCC | RCC (LUI+ODH) / BB | RCC (LUI).
set object 204 rect from screen 0.20,0.635 to screen 0.24,0.665 fc rgb word(COLS,4) fs solid 1.0 border lc rgb "#000000" front
set label 204 "RCC" at screen 0.25,0.650 left font "Helvetica,13" tc rgb "#222222" front
set object 203 rect from screen 0.50,0.635 to screen 0.54,0.665 fc rgb word(COLS,3) fs solid 1.0 border lc rgb "#000000" front
set label 203 "RCC (LUI+ODH)" at screen 0.55,0.650 left font "Helvetica,13" tc rgb "#222222" front
set object 201 rect from screen 0.20,0.560 to screen 0.24,0.590 fc rgb word(COLS,1) fs solid 1.0 border lc rgb "#000000" front
set label 201 "BB" at screen 0.25,0.575 left font "Helvetica,13" tc rgb "#222222" front
set object 202 rect from screen 0.50,0.560 to screen 0.54,0.590 fc rgb word(COLS,2) fs solid 1.0 border lc rgb "#000000" front
set label 202 "RCC (LUI)" at screen 0.55,0.575 left font "Helvetica,13" tc rgb "#222222" front

# Horizontal lines distinguish commit wait; vertical lines mark prevention.
arrow_id = 1000
do for [i=2:5] {
  do for [j=1:4] {
    # [*:*][*:*]: read values above YMAX too; the hatch lines are cut at YMAX.
    stats [*:*][*:*] DATA every ::i::i using (column(5*j+1)) nooutput
    wasted = STATS_mean
    stats [*:*][*:*] DATA every ::i::i using (column(5*j)) nooutput
    prevention = STATS_mean
    stats [*:*][*:*] DATA every ::i::i using (column(5*j-1)) nooutput
    commit = STATS_mean
    if (commit > 0) {
      hatch_count = int(commit/0.05)+1
      do for [h=1:hatch_count] {
        hatch_y = wasted+prevention+commit*h/(hatch_count+1)
        if (hatch_y < YMAX) {
          set arrow arrow_id from first (pos(i,j)-bw/2),first hatch_y to first (pos(i,j)+bw/2),first hatch_y nohead front lw 0.5 lc rgb "#000000"
          arrow_id = arrow_id+1
        }
      }
    }
    if (prevention > 0 && wasted < YMAX) {
      prev_top = (wasted+prevention < YMAX) ? wasted+prevention : YMAX
      do for [h=1:3] {
        hatch_x = pos(i,j)-bw/2+bw*h/4.0
        set arrow arrow_id from first hatch_x,first wasted to first hatch_x,first prev_top nohead front lw 0.5 lc rgb "#000000"
        arrow_id = arrow_id+1
      }
    }
  }
}
do for [h=1:3] {
  set arrow (2000+h) from screen 0.20,screen (0.795+h*0.0075) to screen 0.24,screen (0.795+h*0.0075) nohead front lw 0.5 lc rgb "#000000"
  set arrow (2010+h) from screen (0.50+h*0.01),screen 0.795 to screen (0.50+h*0.01),screen 0.825 nohead front lw 0.5 lc rgb "#000000"
}

plot \
  for [i=2:5] for [j=1:4] DATA every ::i::i using (pos(i,j)):(column(5*j-3)+column(5*j-2)+column(5*j-1)+column(5*j)+column(5*j+1)):(bw) with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=2:5] for [j=1:4] DATA every ::i::i using (pos(i,j)):(column(5*j-2)+column(5*j-1)+column(5*j)+column(5*j+1)):(bw) with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=2:5] for [j=1:4] DATA every ::i::i using (pos(i,j)):(column(5*j-2)+column(5*j-1)+column(5*j)+column(5*j+1)):(bw) with boxes notitle lc rgb "#000000" fs transparent pattern 7 noborder, \
  for [i=2:5] for [j=1:4] DATA every ::i::i using (pos(i,j)):(column(5*j-1)+column(5*j)+column(5*j+1)):(bw) with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=2:5] for [j=1:4] DATA every ::i::i using (pos(i,j)):(column(5*j)+column(5*j+1)):(bw) with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=2:5] for [j=1:4] DATA every ::i::i using (pos(i,j)):(column(5*j+1)):(bw) with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=2:5] for [j=1:4] DATA every ::i::i using (pos(i,j)):(column(5*j+1)):(bw) with boxes notitle lc rgb "#000000" fs transparent pattern 10 noborder
set output
