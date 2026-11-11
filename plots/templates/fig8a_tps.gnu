set terminal pdf size 3,2 font "Helvetica,17"
set output 'fig8a_tps.pdf'
set datafile separator ","

set xlabel "Number of Threads"
set ylabel "Throughput (TPS)"

# Use smaller legend text to fit two columns with the common sample length.
set key reverse Left samplen 3 font "Helvetica,13"

set grid ytics
set xtics out
set ytics out
set tics scale 0.5

set xrange [-0.3:3.3]


set yrange [100000:600000]
set ytics ("0.2M" 200000, "0.4M" 400000, "0.6M" 600000)
set xtics nomirror
set ytics nomirror

set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7 dt 2 # RCC (LUI)
set style line 21 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7 dt 4 # RCC
set style line 41 lc rgb "#699158" lw 1 pt 9 ps 0.7 dt 5 # BB
set style line 51 lc rgb "#637cb3" lw 1 pt 13 ps 0.7 dt 3 # RCC (LUI+ODH)


# This CSV stores thousands of transactions per second; convert only for display.
# Skip the header and the 1- and 16-thread rows; show 32 through 128.
plot \
  'tps_128.csv' skip 3 using 0:($5*1000) with linespoints ls 21 title "RCC" at 0.38,0.835, \
  'tps_128.csv' skip 3 using 0:($4*1000) with linespoints ls 51 title "RCC (LUI+ODH)" at 0.60,0.835, \
  'tps_128.csv' skip 3 using 0:($3*1000) with linespoints ls 11 title "RCC (LUI)" at 0.60,0.733, \
  'tps_128.csv' skip 3 using 0:($2*1000):xtic(1) with linespoints ls 41 title "BB" at 0.38,0.733
set output
