CC=gcc

BASE_DIR=$(shell pwd)
TEST_DIR=$(BASE_DIR)/test
BIN_DIR=$(BASE_DIR)/bin

IFLAGS=-I$(BASE_DIR)/include

DFLAGS=-DBLAKE3_NO_AVX512

DEBUG=0

ifeq ($(DEBUG), 0)
CFLAGS=-O3 -std=c11 -mavx2 -Wall -Wno-unused-function -MMD -MP $(DFLAGS)
else
CFLAGS=-O0 -std=c11 -mavx2 -Wall -Wno-unused-function -MMD -MP -g3 -DDEBUG $(DFLAGS)
endif

LDFLAGS=-flto

