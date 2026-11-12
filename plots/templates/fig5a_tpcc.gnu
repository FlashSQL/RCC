set terminal pdf size 3,2 font "Helvetica,17"
set output 'fig5a_tpcc.pdf'
set datafile separator ","

set xlabel "Number of Threads"  
set ylabel "Throughput (TPS)"

set key inside left top horizontal maxcols 2 maxrows 3 reverse Left samplen 3 spacing 1.3 width 1 font "Helvetica,17"

set grid ytics
set xtics out
set ytics out
set tics scale 0.5

set lmargin 8.5
set xrange [-0.3:5.3]


set yrange [0:40000]
set ytics ("0" 0, "20K" 20000, "40K" 40000)
set xtics nomirror
set ytics nomirror

# Reference line: improvement ratio = 1 (same performance)
set style line 90 lc rgb "#888888" dt 2 lw 1.5

set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7  dt 2# VAN
set style line 61 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7 dt 1  # RCC
set style line 31 lc rgb "#de556f" lw 1 pt 13 ps 0.7  # RCC-C
set style line 41 lc rgb "#699158" lw 1 pt 9 ps 0.7  dt 5 # BB
set style line 51 lc rgb "#637cb3" lw 1 pt 1 ps 0.7  dt 5 # RR
# Red FF5500
# Yellow ffff00


plot \
  'tps_128.csv' using 0:6 with linespoints ls 31 title "RCC-C",  \
  'tps_128.csv' using 0:4 with linespoints ls 51 title "RR",  \
  'tps_128.csv' using 0:2:xtic(1) with linespoints ls 11 title "VAN",  \
  'tps_128.csv' using 0:5 with linespoints ls 61 title "RCC",  \
  'tps_128.csv' using 0:3 with linespoints ls 41 title "BB"


set output
