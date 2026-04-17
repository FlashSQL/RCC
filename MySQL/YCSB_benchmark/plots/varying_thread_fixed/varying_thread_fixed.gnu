set terminal pdf size 3,2 font "Helvetica,16"
set output 'varying_thread_fixed.pdf'
set datafile separator ","

set xlabel "Number of Threads"
set ylabel "Throughput (TPS)"

set key inside left top reverse Left samplen 3 font "Helvetica, 14"

set grid ytics
set xtics out
set ytics out
set tics scale 0.5

set xrange [-0.3:5.3]

set yrange [0:30000]
set ytics ("0" 0, "10K" 10000, "20K" 20000)
set xtics nomirror
set ytics nomirror

set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7 dt 2       # VAN
set style line 21 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7 dt 4     # RCC
set style line 41 lc rgb "#699158" lw 1 pt 9 ps 0.7 dt 5      # RCC(W)
set style line 51 lc rgb "#637cb3" lw 1 pt 1 ps 0.7 dt 5      # RCC(D)

plot \
  'varying_thread_fixed.csv' using 0:5 with linespoints ls 21 title "RCC",  \
  'varying_thread_fixed.csv' using 0:3 with linespoints ls 51 title "RCC(D)",  \
  'varying_thread_fixed.csv' using 0:4 with linespoints ls 41 title "RCC(W)",  \
  'varying_thread_fixed.csv' using 0:2:xtic(1) with linespoints ls 11 title "VAN"

# -----------------------------
# Plot 2: Abort Rate
# -----------------------------
set output 'varying_thread_fixed_abort.pdf'
set ylabel "Abort Rate (%)"
set yrange [0:120]
set ytics 0,25,100
set key inside left top reverse Left samplen 3 font "Helvetica, 14"

plot \
  'abort_rate.csv' using 0:5 with linespoints ls 21 title "RCC",  \
  'abort_rate.csv' using 0:3 with linespoints ls 51 title "RCC(D)",  \
  'abort_rate.csv' using 0:4 with linespoints ls 41 title "RCC(W)",  \
  'abort_rate.csv' using 0:2:xtic(1) with linespoints ls 11 title "VAN"
set output
