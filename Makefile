EXECS = lbard manifesttest fakecsmaradio

all:	$(EXECS)

clean:
	rm -rf src/version.h $(EXECS) echotest

SRCS=	src/util.c src/main.c src/rhizome.c src/txmessages.c src/rxmessages.c src/bundle_cache.c src/json.c src/peers.c \
	src/serial.c src/radio.c src/golay.c src/httpclient.c src/progress.c src/rank.c src/bundles.c src/partials.c \
	src/manifests.c src/monitor.c src/timesync.c src/httpd.c src/meshms.c \
	src/status_dump.c \
	fec-3.0.1/ccsds_tables.c \
	fec-3.0.1/encode_rs_8.c \
	fec-3.0.1/init_rs_char.c \
	fec-3.0.1/decode_rs_8.c \
	src/bundle_tree.c src/sha1.c src/sync.c \
	src/drivers/hfcontroller.c src/drivers/uhfcontroller.c src/drivers/rfcontroller.c

HDRS=	src/lbard.h src/serial.h Makefile src/version.h src/sync.h src/util.h
#CC=/usr/local/Cellar/llvm/3.6.2/bin/clang
#LDFLAGS= -lgmalloc
#CFLAGS= -fno-omit-frame-pointer -fsanitize=address
#CC=clang
#LDFLAGS= -lefence
LDFLAGS=
CFLAGS= -g -std=gnu99 -Wall -fno-omit-frame-pointer -D_GNU_SOURCE=1 -Isrc/ -I.

version.h:	$(SRCS)
	echo "#define VERSION_STRING \""`./md5 $(SRCS)`"\"" >src/version.h

lbard:	version.h $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o lbard $(SRCS) $(LDFLAGS)

echotest:	Makefile extra/echotest.c
	$(CC) $(CFLAGS) -o echotest extra/echotest.c

fakecsmaradio:	Makefile extra/fakecsmaradio.c
	$(CC) $(CFLAGS) -o fakecsmaradio extra/fakecsmaradio.c

manifesttest:	Makefile src/manifests.c
	$(CC) $(CFLAGS) -DTEST -o manifesttest src/manifests.c src/util.c
