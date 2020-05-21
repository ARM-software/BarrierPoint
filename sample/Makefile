CC = gcc

OPTFLAGS = -g -O3
CFLAGS = $(OPTFLAGS) -fopenmp -Wall

all:
	$(CC) $(CFLAGS) matrixmul.c $(LDFLAGS) -o matrixmul

clean:
	rm -f *.o matrixmul
