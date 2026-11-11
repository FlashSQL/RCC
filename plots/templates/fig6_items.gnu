set terminal pdf size 3,2 font "Helvetica,18"
set output 'fig6a_tps.pdf'
set datafile separator ","

set xlabel "Number of Items"  
set ylabel "Throughput (TPS)"

set key inside center top horizontal maxrows 1 reverse Left samplen 3 width 0 font "Helvetica,13"
# Entries are placed with "at" so the gaps between them are equal.

set grid ytics
set xtics out
set ytics out
set tics scale 0.5
# Fixed left margin so the TPS and abort plot boxes have the same size.
set lmargin at screen 0.268

set xrange [4.3:-0.3]


set yrange [0:36000]
set ytics ("0" 0, "15K" 15000, "30K" 30000)
set xtics ("0.1K" 0, "1K" 1, "10K" 2, "100K" 3, "10M" 4)
set xtics nomirror
set ytics nomirror

# Reference line: improvement ratio = 1 (same performance)
set style line 90 lc rgb "#888888" dt 2 lw 1.5


set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7  dt 2# VAN
set style line 61 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7 dt 1  # RCC
set style line 31 lc rgb "#de556f" lw 1 pt 13 ps 0.7  # RCC-C
set style line 41 lc rgb "#699158" lw 1 pt 9 ps 0.7  dt 5 # BB
set style line 51 lc rgb "#637cb3" lw 1 pt 1 ps 0.7  dt 5 # RR


plot \
  'tps_128.csv' every ::0::4 using 0:4 with linespoints ls 61 title "RCC" at screen 0.4029,0.8525,  \
  'tps_128.csv' every ::0::4 using 0:3 with linespoints ls 51 title "RR" at screen 0.6304,0.8525,  \
  'tps_128.csv' every ::0::4 using 0:2 with linespoints ls 41 title "BB" at screen 0.8291,0.8525


# -----------------------------
# Plot 2: Abort Rate
# -----------------------------
set output 'fig6b_abort.pdf'
set ylabel "Abort Rate (%)"
set xrange [4.3:-0.3]
set yrange [0:130]
set ytics 0,50,100
set key inside center top horizontal maxrows 1 reverse Left samplen 3 width 0 font "Helvetica,13"
# Entries are placed with "at" so the gaps between them are equal.

plot \
  'abort_128.csv' every ::0::4 using 0:4 with linespoints ls 61 title "RCC" at screen 0.4029,0.8525,  \
  'abort_128.csv' every ::0::4 using 0:3 with linespoints ls 51 title "RR" at screen 0.6304,0.8525,  \
  'abort_128.csv' every ::0::4 using 0:2 with linespoints ls 41 title "BB" at screen 0.8291,0.8525
set output
