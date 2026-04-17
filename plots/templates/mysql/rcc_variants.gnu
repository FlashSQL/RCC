set terminal pdf size 3,2 font "Helvetica,16"
set datafile separator ","

set xlabel "Number of Threads"
set ylabel "Throughput (TPS)"

set key inside left top reverse Left samplen 3 font "Helvetica, 14"

set grid ytics
set xtics out
set ytics out
set tics scale 0.5

set xrange [-0.3:5.3]
set xtics nomirror
set ytics nomirror

set style line 90 lc rgb "#888888" dt 2 lw 1.5
set style line 11 lc rgb "black"   lw 1 pt 7  ps 0.7 dt 2  # VAN
set style line 21 lc rgb "#ff7f0e" lw 1 pt 5  ps 0.7 dt 4  # RCC
set style line 31 lc rgb "#de556f" lw 1 pt 13 ps 0.7       # RCC-C
set style line 41 lc rgb "#699158" lw 1 pt 9  ps 0.7 dt 5  # RCC-W
set style line 51 lc rgb "#637cb3" lw 1 pt 1  ps 0.7 dt 5  # RCC-D

# ========== Repeatable Read ==========
set output 'rcc_variants_rr.pdf'
set yrange [0:30000]
set ytics ("0" 0, "10K" 10000, "20K" 20000, "30K" 30000)
plot \
  'varying_conn_rr.csv' using 0:5 with linespoints ls 31 title "RCC-C", \
  'varying_conn_rr.csv' using 0:4 with linespoints ls 21 title "RCC", \
  'varying_conn_rr.csv' using 0:6 with linespoints ls 51 title "RCC-D", \
  'varying_conn_rr.csv' using 0:3 with linespoints ls 41 title "RCC-W", \
  'varying_conn_rr.csv' using 0:2:xtic(1) with linespoints ls 11 title "VAN"

# ========== Serializable ==========
set output 'rcc_variants_sr.pdf'
set yrange [0:25000]
set ytics ("0" 0, "10K" 10000, "20K" 20000)
plot \
  'varying_conn_sr.csv' using 0:5 with linespoints ls 31 title "RCC-C", \
  'varying_conn_sr.csv' using 0:4 with linespoints ls 21 title "RCC", \
  'varying_conn_sr.csv' using 0:6 with linespoints ls 51 title "RCC-D", \
  'varying_conn_sr.csv' using 0:3 with linespoints ls 41 title "RCC-W", \
  'varying_conn_sr.csv' using 0:2:xtic(1) with linespoints ls 11 title "VAN"
