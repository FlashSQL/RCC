set terminal pdf size 6,2 font "Helvetica,16"
set output 'combined.pdf'
set multiplot
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
set lmargin at screen 0.115
set rmargin at screen 0.49
set tmargin at screen 0.96
set bmargin at screen 0.25
set xlabel offset 0,1
set xtics offset 0,0.5 font "Helvetica,11"
set ylabel 'Throughput (K TPS)' font "Helvetica,12" offset 1.2,0
set yrange [0:12]
set ytics 3
plot 'tps.csv' using 0:4 with linespoints ls 5 title 'RCC-RR', \
'tps.csv' using 0:3 with linespoints ls 1 title 'VAN-RR', \
'tps.csv' using 0:2:xtic(1) with linespoints ls 2 title 'VAN-RC'
reset
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
set lmargin at screen 0.615
set rmargin at screen 0.99
set tmargin at screen 0.96
set bmargin at screen 0.25
set xlabel offset 0,1
set xtics offset 0,0.5 font "Helvetica,11"
set ylabel 'Abort Rate (%)' font "Helvetica,12" offset 1.2,0
set yrange [0:100]
set ytics 20
set key at graph 0.60, graph 0.80 left top reverse Left samplen 2 spacing 0.9 font 'Helvetica,10'
plot 'abort.csv' using 0:4 with linespoints ls 5 title 'RCC-RR', \
'abort.csv' using 0:3 with linespoints ls 1 title 'VAN-RR', \
'abort.csv' using 0:2:xtic(1) with linespoints ls 2 title 'VAN-RC'
unset multiplot
set output
