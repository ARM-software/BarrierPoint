all: simpoint sample BPlibs

# Set custom PAPI directory, if needed
PAPI_DIR ?=

BPlibs:
	make PAPI_DIR=$(PAPI_DIR) -C barrierpoint-libs

simpoint:
	if [ ! -d SimPoint.3.2 ]; then\
		wget -O - http://cseweb.ucsd.edu/~calder/simpoint/releases/SimPoint.3.2.tar.gz | tar -x -f - -z;\
		patch -p0 < simpoint.patch;\
	fi
	make -C SimPoint.3.2
	ln -s SimPoint.3.2/bin/simpoint ./simpoint

microbenchmarks:
	make -C sample

clean:
	make -C sample clean
	make -C barrierpoint-libs clean
	rm ./simpoint

distclean: clean
	rm -rf SimPoint.3.2

.PHONY: clean distclean sample
