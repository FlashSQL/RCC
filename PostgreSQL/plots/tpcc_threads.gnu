set terminal pdf size 3,2 font "Helvetica,17"
set output 'tpcc_threads.pdf'
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

set yrange [0:12000]
set ytics ("0" 0, "6K" 6000, "12K" 12000)
set xtics nomirror
set ytics nomirror

set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7 dt 2    # VAN-RC
set style line 51 lc rgb "#637cb3" lw 1 pt 1 ps 0.7 dt 5  # VAN-RR
set style line 61 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7 dt 1  # RCC-RR

plot \
  'tps_128.csv' using 0:4 with linespoints ls 61 title "RCC-RR", \
  'tps_128.csv' using 0:3 with linespoints ls 51 title "VAN-RR", \
  'tps_128.csv' using 0:2:xtic(1) with linespoints ls 11 title "VAN-RC"
set output
