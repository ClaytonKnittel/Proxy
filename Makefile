include common.mk

SDIR=$(BASE_DIR)/src
ODIR=$(BASE_DIR)/.obj

EXE=$(BIN_DIR)/proxy


SRC=$(shell find $(SDIR) -type f -name '*.c')
OBJ=$(patsubst $(SDIR)/%.c,$(ODIR)/%.o,$(SRC))
DEP=$(wildcard $(SDIR)/*.h)

DIRS=$(shell find $(SDIR) -type d)
OBJDIRS=$(patsubst $(SDIR)/%,$(ODIR)/%,$(DIRS))

$(shell mkdir -p $(ODIR))
$(shell mkdir -p $(OBJDIRS))
$(shell mkdir -p $(BIN_DIR))

DEPFILES=$(SRC:$(SDIR)/%.c=$(ODIR)/%.d)


.PHONY: all
all: $(EXE) tests

.PHONY: tests
tests:
	(make -C $(TEST_DIR) BASE_DIR=$(BASE_DIR) EXT_OBJ="$(OBJ)")


$(EXE): $(OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFALGS)


$(ODIR)/%.o: $(SDIR)/%.c
	$(CC) $(CFLAGS) $< -c -o $@ $(IFLAGS)


-include $(wildcard $(DEPFILES))

.PHONY: clean
clean:
	rm -rf $(ODIR)
	rm -rf $(BIN_DIR)
	(make -C $(TEST_DIR) clean)


