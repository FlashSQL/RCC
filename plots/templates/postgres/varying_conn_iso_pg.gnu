# Fig6: TPCC-PG throughput vs. client threads (1 warehouse)
# VAN-RC / VAN-RR / RCC-RR
if (!exists("TERM_SET")) {
  set terminal pdf size 3,2 font "Helvetica,9"
  set output 'varying_clients_threads_pg.pdf'
  KEY_FONT = "Helvetica,8"
} else {
  KEY_FONT = "Helvetica,14"
}
set datafile separator ","
set datafile missing "NaN"

set xlabel "Number of Threads"
set ylabel "Throughput (TPS)"

set key inside left top reverse Left samplen 3 font KEY_FONT

set grid ytics
set xtics out
set ytics out
set tics scale 0.5

stats 'varying_conn_pg.csv' using 0 nooutput
set xrange [-0.3:STATS_records-0.7]

set yrange [0:20000]
set ytics ("0" 0, "5K" 5000, "10K" 10000, "15K" 15000, "20K" 20000)
set xtics nomirror
set ytics nomirror

set style line 11 lc rgb "black"   lw 1 pt 7 ps 0.7 dt 2  # VAN-RR
set style line 12 lc rgb "#1f77b4" lw 1 pt 9 ps 0.7 dt 4  # VAN-RC
set style line 21 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7       # RCC-RR

plot \
  'varying_conn_pg.csv' using 0:4 with linespoints ls 21 title "RCC-RR", \
  '' using 0:3 with linespoints ls 11 title "VAN-RR", \
  '' using 0:2:xtic(1) with linespoints ls 12 title "VAN-RC"
