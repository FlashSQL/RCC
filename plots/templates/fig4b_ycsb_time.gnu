set terminal pdf size 3,2 font "Helvetica,17"
set output 'fig4b_ycsb_time.pdf'
DATA = 'time_wait_split_128.dat'
set ylabel "Amortized Runtime (ms)" offset 0,0
set xlabel "Number of Threads" offset 0,1
set xrange [0:30]
set yrange [0:40]
set ytics ("0" 0, "10" 10, "20" 20, "30" 30, "40" 40) font "Helvetica,17"
set xtics ("1" 2.5, "16" 7.5, "32" 12.5, "64" 17.5, "96" 22.5, "128" 27.5) font "Helvetica,17"
set xtics offset 0,0.5 out nomirror
set ytics out nomirror
set tics scale 0.5
set grid ytics lt 1 lc rgb "#dddddd"
set border 2+4+8
# Same plot box as the dbx1000 ablation time graph.
set lmargin at screen (39.501/215.0)
set rmargin at screen (197.151/215.0)
set tmargin at screen (132.351/143.0)
set bmargin at screen (28.0/143.0)
unset key
COLS = "#b0b0b0 #a0d084 #9cb2e3 #ff7f0e"
pos(i,j) = 5*i + j
bw = 1.0
# Four columns per system: Execution, Lock Wait, Commit Wait, Wasted.
# Bottom to top: Wasted, Commit Wait, Lock Wait, Execution.
# Legend layout follows the dbx1000 ablation time graph.
set object 211 rect from screen 0.200,0.870 to screen 0.240,0.900 fc rgb "#ffffff" fs solid 1.0 border lc rgb "#000000" front
set label 211 "Execution" at screen 0.250,0.885 left font "Helvetica,16" tc rgb "#222222" front
set object 212 rect from screen 0.600,0.870 to screen 0.640,0.900 fc rgb "#000000" fs transparent pattern 7 border lc rgb "#000000" front
set label 212 "Lock Wait" at screen 0.650,0.885 left font "Helvetica,16" tc rgb "#222222" front
set object 213 rect from screen 0.200,0.780 to screen 0.240,0.810 fc rgb "#ffffff" fs solid 1.0 border lc rgb "#000000" front
set label 213 "Commit Wait" at screen 0.250,0.795 left font "Helvetica,16" tc rgb "#222222" front
set object 214 rect from screen 0.600,0.780 to screen 0.640,0.810 fc rgb "#000000" fs transparent pattern 10 border lc rgb "#000000" front
set label 214 "Wasted" at screen 0.650,0.795 left font "Helvetica,16" tc rgb "#222222" front

# System legend, read left to right: RCC | RR / BB | VAN (same as the TPS graph).
set object 206 rect from screen 0.200,0.680 to screen 0.240,0.710 fc rgb word(COLS,4) fs solid 1.0 border lc rgb "#000000" front
set label 206 "RCC" at screen 0.250,0.695 left font "Helvetica,16" tc rgb "#222222" front
set object 202 rect from screen 0.200,0.590 to screen 0.240,0.620 fc rgb word(COLS,2) fs solid 1.0 border lc rgb "#000000" front
set label 202 "BB" at screen 0.250,0.605 left font "Helvetica,16" tc rgb "#222222" front
set object 203 rect from screen 0.600,0.680 to screen 0.640,0.710 fc rgb word(COLS,3) fs solid 1.0 border lc rgb "#000000" front
set label 203 "RR" at screen 0.650,0.695 left font "Helvetica,16" tc rgb "#222222" front
set object 201 rect from screen 0.600,0.590 to screen 0.640,0.620 fc rgb word(COLS,1) fs solid 1.0 border lc rgb "#000000" front
set label 201 "VAN" at screen 0.650,0.605 left font "Helvetica,16" tc rgb "#222222" front
# Horizontal hatching distinguishes Commit Wait from diagonal Lock Wait.
arrow_id = 1000
do for [i=0:5] {
  do for [j=1:4] {
    stats DATA every ::i::i using (column(4*j+1)) nooutput
    wasted = STATS_mean
    stats DATA every ::i::i using (column(4*j)) nooutput
    commit = STATS_mean
    if (commit > 0) {
      hatch_count = int(commit/(40.0*1.5/70.0))+1
      do for [h=1:hatch_count] {
        hatch_y = wasted + commit*h/(hatch_count+1)
        set arrow arrow_id from first (pos(i,j)-bw/2), first hatch_y to first (pos(i,j)+bw/2), first hatch_y nohead front lw 0.5 lc rgb "#000000"
        arrow_id = arrow_id+1
      }
    }
  }
}
do for [h=1:3] {
  set arrow (2000+h) from screen 0.20, screen (0.780+h*0.0075) to screen 0.24, screen (0.780+h*0.0075) nohead front lw 0.5 lc rgb "#000000"
}
plot \
  for [i=0:5] for [j=1:4] DATA using (pos(i,j)):(column(4*j-2)+column(4*j-1)+column(4*j+0)+column(4*j+1)):(bw) every ::i::i with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=0:5] for [j=1:4] DATA using (pos(i,j)):(column(4*j-1)+column(4*j+0)+column(4*j+1)):(bw) every ::i::i with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=0:5] for [j=1:4] DATA using (pos(i,j)):(column(4*j-1)+column(4*j+0)+column(4*j+1)):(bw) every ::i::i with boxes notitle lc rgb "#000000" fs transparent pattern 7 noborder, \
  for [i=0:5] for [j=1:4] DATA using (pos(i,j)):(column(4*j+0)+column(4*j+1)):(bw) every ::i::i with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=0:5] for [j=1:4] DATA using (pos(i,j)):(column(4*j+1)):(bw) every ::i::i with boxes notitle lc rgb word(COLS,j) fs solid 1.0 border lc rgb "#000000", \
  for [i=0:5] for [j=1:4] DATA using (pos(i,j)):(column(4*j+1)):(bw) every ::i::i with boxes notitle lc rgb "#000000" fs transparent pattern 10 noborder
set output
