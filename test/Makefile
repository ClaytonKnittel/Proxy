include ../common.mk

ODIR=.obj

SRC=$(shell find . -type f -name '*.c')
OBJ=$(patsubst %.c,$(ODIR)/%.o,$(SRC))
EXE=$(patsubst %.c,$(BIN_DIR)/%,$(SRC))

$(shell mkdir -p $(ODIR))

DEPFILES=$(SRC:%.c=$(ODIR)/%.d)


# to prevent files from being auto-deleted by make after build completes
.SECONDARY:

.PHONY: all
all: $(EXE)

$(BIN_DIR)/%: $(ODIR)/%.o $(EXT_OBJ)
	$(CC) $(CFLAGS) $< $(EXT_OBJ) -o $@ $(IFLAGS) $(LDFLAGS)

$(ODIR)/%.o: %.c
	$(CC) $(CFLAGS) $< -c -o $@ $(IFLAGS)


-include $(wildcard $(DEPFILES))

.PHONY: clean
clean:
	rm -rf $(ODIR)

