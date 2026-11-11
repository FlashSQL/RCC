set terminal pdf size 3,2 font "Helvetica,17"
set output 'fig7b_tpcc.pdf'
set datafile separator ","

set xlabel "Number of Threads"
set ylabel "Throughput (TPS)"

set key inside left top horizontal maxcols 2 maxrows 2 reverse Left samplen 3 spacing 1.3 width 1 font "Helvetica,17"

set grid ytics
set xtics out
set ytics out
set tics scale 0.5

set lmargin 8.5
set xrange [-0.3:5.3]


set yrange [0:800000]
set ytics ("0" 0, "0.4M" 400000, "0.8M" 800000)
set xtics nomirror
set ytics nomirror

set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7  dt 2# WW
set style line 21 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7 dt 4  # RCC
set style line 41 lc rgb "#699158" lw 1 pt 9 ps 0.7  dt 5 # BB
set style line 51 lc rgb "#637cb3" lw 1 pt 13 ps 0.7 dt 3 # Silo


plot \
  'tps_128.csv' skip 1 using 0:4 with linespoints ls 21 title "RCC",  \
  'tps_128.csv' skip 1 using 0:3 with linespoints ls 41 title "BB",  \
  'tps_128.csv' skip 1 using 0:5 with linespoints ls 51 title "Silo", \
  'tps_128.csv' skip 1 using 0:2:xtic(1) with linespoints ls 11 title "WW"
set output
