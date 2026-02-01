set terminal pdf size 3,2 font "Helvetica,16"
set output 'varying_wh_tpcc_my.pdf'
set datafile separator ","

set xlabel "Number of Warehouses"  
set ylabel "Throughput (TPS)"

set grid ytics
set key inside left top reverse Left samplen 3 font "Helvetica, 14"

set xrange [-0.3:3.3]

set yrange [0:30000]
set ytics 10000
set ytics ("0" 0, "15K" 15000, "30K" 30000)
set xtics out 
set ytics out
set tics scale 0.5
set xtics nomirror
set ytics nomirror

# 참고용 기준선: 개선율 = 1 (동일 성능)
set style line 90 lc rgb "#888888" dt 2 lw 1.5

set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7  dt 2# VAN
set style line 21 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7  # RCC

plot \
  'varying_wh_my.csv' using 0:3 with linespoints ls 21 title "RCC",  \
  '' using 0:2:xtic(1) with linespoints ls 11 title "VAN"