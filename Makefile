CC = gcc
FLAGS = -O3

all: fflz4.c
	$(CC) fflz4.c ./lib/lz4/lz4.c ./lib/cJSON/cJSON.c $(FLAGS) -o fflz4
