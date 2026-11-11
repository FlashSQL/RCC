# Override MYSQL_CONFIG when using a different MySQL 8.4 client installation.
MYSQL_CONFIG ?= ../../Vanilla/mysql-8.4.5-install/bin/mysql_config
CFLAGS = -w -O3 -g -I. $(shell $(MYSQL_CONFIG) --include)
LDFLAGS = $(shell $(MYSQL_CONFIG) --libs) -lpthread -lz -lm -ldl -lrt

TRANSACTIONS = neword.o payment.o ordstat.o delivery.o slev.o
OBJS = main.o spt_proc.o driver.o support.o sequence.o rthist.o sb_percentile.o $(TRANSACTIONS)

.SUFFIXES:
.SUFFIXES: .o .c

.c.o:
	$(CC) $(CFLAGS) -c $*.c

all: ../tpcc_load ../tpcc_start

# Rebuild both programs when the shared scale parameters change.
$(OBJS) load.o: tpc.h

../tpcc_load: load.o support.o
	$(CC) load.o support.o $(LDFLAGS) -o ../tpcc_load

../tpcc_start: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o ../tpcc_start

clean:
	rm -f *.o ../tpcc_load ../tpcc_start
