set terminal pdfcairo size 3,2 font "Helvetica, 16"
set output 'single_hot_mini_my.pdf'
set datafile separator ","

set xlabel "Number of Threads" 
set ylabel "Speedup"
set grid ytics

set xtics out
set ytics out
set tics scale 0.5
set key inside left top reverse Left samplen 2.5 font "Helvetica, 14"

# x축을 등간격으로: 데이터 행 인덱스($0)를 x로 사용하고,
# 눈금 라벨은 1/2/4/8/16/32(=1열)로 표시
set xrange [-0.3:5.3]
# 첫 시리즈에서만 xtic(1)로 라벨 부착 (다른 시리즈는 생략)
# 참고: xtic(1)은 1열 값을 문자열로 라벨링

set yrange [-5:100]
set ytics 40

set xtics nomirror
set ytics nomirror

# 참고용 기준선: 개선율 = 1 (동일 성능)
set style line 90 lc rgb "#888888" dt 2 lw 1.5

# 색상: 32=파랑, 64=주황, 128=초록, 256=노랑
# 마커: 32=원(pt 7), 64=세모(pt 9), 128=원(pt 7), 256=네모(pt 5)
set style line 11 lc rgb "black" lw 2 pt 9 ps 0.7 dt 2  # 64 reads
set style line 21 lc rgb "#1f77b4" lw 2 pt 7 ps 0.7 dt 4 # 128 reads
set style line 31 lc rgb "#ff7f0e" lw 2 pt 5 ps 0.7  # 256 reads 

plot \
  'ratio_by_threads_my.csv' using 0:2:xtic(1) with linespoints ls 11 title "{/Times-Italic Len}=64", \
  '' using 0:3 with linespoints ls 21 title "{/Times-Italic Len}=128", \
  '' using 0:4 with linespoints ls 31 title "{/Times-Italic Len}=256"