reset
set encoding utf8

# -----------------------------
# Data blocks
# -----------------------------
$throughput << EOD
Threads SILO    TicToc  WOUND_WAIT  BAMBOO  REBIRTH
1   156975 	156352 	203997 	178357 	147325 
2   948493 	953695 	524913 	826208 
3   972732 	967834 	347509 	509406 
4   760824 	761142 	289113 	207154 
5   536290 	531461 	240101 	27339 	1275 
6   324882 	379008 	174080 	2135 	2079 
EOD

$abort << EOD
Threads SILO    TicToc  WOUND_WAIT  BAMBOO  REBIRTH
1   0	0	0	0	0
2   48	45	32	32
3   69	67	45	58
4   88	87	57	85
5   94	94	66	98	98
6   97	96	74	99	98
EOD

# -----------------------------
# Common style settings
# -----------------------------
set datafile separator whitespace
set terminal pdf size 3,2 font "Helvetica,16"

set xtics ("1" 1, "8" 2, "16" 3, "32" 4, "64" 5, "128" 6)
set xrange [0.7:6.3]
set xlabel "Number of Threads"  
set xtics nomirror
set ytics nomirror
set grid ytics
set xtics out
set ytics out


# -----------------------------
# Line/point styles matched to the reference figure
# -----------------------------
# 1: Bamboo  = orange square
# 2: Wound-Wait    = gray x
# 3: Rebirth-Retire= yellow filled circle
# 5: Silo          = green x
# 6: TicToc        = dark-blue filled triangle

set style line 11 lc rgb "black" lw 1 pt 7 ps 0.7 dt 4 # Wound-Wait
set style line 21 lc rgb "#ff7f0e" lw 1 pt 5 ps 0.7 dt 3  # TicToc
set style line 51 lc rgb "#637cb3" lw 1 pt 1 ps 0.7  dt 3 # Silo
set style line 41 lc rgb "#699158" lw 1 pt 9 ps 0.7  dt 2 # Bamboo
set style line 31 lc rgb "#de556f" lw 1 pt 13 ps 0.7 dt 2  # Rebirth-Retire

# -----------------------------
# Plot 1: Throughput
# -----------------------------
set output "tps_ycsb_cc_99.pdf"
unset format y
set ylabel "Throughput (TPS)"
set yrange [0:1400000]
set ytics ("0" 0, "1M" 1000000, "2M" 2000000)
set key right top Right samplen 2 at graph 0.98, graph 0.97 font "Helvetica, 14"
plot \
    $throughput using 1:6 with linespoints ls 31 title "Rebirth-Retire", \
    $throughput using 1:4 with linespoints ls 11 title "Wound-Wait", \
    $throughput using 1:5 with linespoints ls 41 title "Bamboo", \
    $throughput using 1:3 with linespoints ls 21 title "TicToc", \
    $throughput using 1:2 with linespoints ls 51 title "Silo"
set output

# -----------------------------
# Plot 2: Abort Rate
# Current datablock has no SILO column, so Silo is omitted here.
# If you add SILO later, insert it between DL-Detection and TicToc:
#   $abort_rate using 1:8 with linespoints ls 5 title "Silo", \
# -----------------------------
set output "abort_ycsb_cc_99.pdf"
set ylabel "Abort Rate (%)"
set yrange [0:120]
set ytics 0,50,100
set key inside left top reverse Left samplen 2 at graph 0.01, graph 0.97 font "Helvetica, 14"

plot \
    $abort using 1:6 with linespoints ls 31 title "Rebirth-Retire", \
    $abort using 1:4 with linespoints ls 11 title "Wound-Wait", \
    $abort using 1:5 with linespoints ls 41 title "Bamboo", \
    $abort using 1:3 with linespoints ls 21 title "TicToc", \
    $abort using 1:2 with linespoints ls 51 title "Silo"
set output
