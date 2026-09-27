set terminal pdf size 3,2 font "Helvetica,16"
set datafile separator ","
set datafile missing ""
set grid ytics
set xtics out nomirror font 'Helvetica,12'
set ytics out nomirror font 'Helvetica,12'
set tics scale 0.5
set key inside left top reverse Left samplen 2 spacing 0.9 font 'Helvetica,10'
set style line 1 lc rgb 'black' lw 2 pt 7 ps .7
set style line 2 lc rgb '#637cb3' lw 2 pt 9 ps .7
set style line 5 lc rgb '#ff7f0e' lw 2 pt 5 ps .7
set xrange [-.3:6.3]
set xlabel 'Number of Threads'
set output 'tps.pdf'
set ylabel 'Throughput (K TPS)'
set yrange [0:12]
set ytics 3
plot 'tps.csv' using 0:4 with linespoints ls 5 title 'RCC-RR', \
'tps.csv' using 0:3 with linespoints ls 1 title 'VAN-RR', \
'tps.csv' using 0:2:xtic(1) with linespoints ls 2 title 'VAN-RC'
set output 'abort.pdf'
set ylabel 'Abort Rate (%)'
set yrange [0:100]
set ytics 20
set key at graph 0.60, graph 0.80 left top reverse Left samplen 2 spacing 0.9 font 'Helvetica,10'
plot 'abort.csv' using 0:4 with linespoints ls 5 title 'RCC-RR', \
'abort.csv' using 0:3 with linespoints ls 1 title 'VAN-RR', \
'abort.csv' using 0:2:xtic(1) with linespoints ls 2 title 'VAN-RC'
set output
