# the `-Wno`s quiet C90 warnings
PG_CFLAGS = -std=c11 -Wextra -Wall -Werror \
	-Wold-style-definition \
	-Wno-declaration-after-statement \
	-Wno-vla \
	-Wno-long-long \
	-Wpedantic -Wshadow -Wconversion -Wsign-conversion \
	-Wstrict-prototypes -Wcast-qual -Wcast-align \
	-Wformat=2 -Wnull-dereference -Wundef -Wpointer-arith \
	-Wswitch-enum -Wimplicit-fallthrough -Wdouble-promotion \
	-Wfloat-equal -Wwrite-strings

ifeq ($(CC),gcc)
  GCC_MAJ := $(firstword $(subst ., ,$(shell $(CC) -dumpfullversion -dumpversion)))
  GCC_GE14 = $(shell test $(GCC_MAJ) -ge 14; echo $$?)
  ifeq ($(GCC_GE14),0)
    PG_CFLAGS += -Wmissing-variable-declarations
  endif
endif

EXTENSION = pg_http
DATA = $(wildcard sql/*--*.sql)
MODULE_big = $(EXTENSION)
HEADERS = $(wildcard src/*.h)
SOURCES = $(wildcard src/*.c)

ifdef BUILD_DIR
OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SOURCES))
else
OBJS = $(patsubst src/%.c, src/%.o, $(SOURCES)) # if no BUILD_DIR, just build on src so standard PGXS `make` works
endif

SHLIB_LINK = -lcurl

PG_CONFIG = pg_config

# treat PostgreSQL headers as system headers so the strict warnings only apply to our code
PG_CPPFLAGS = -isystem $(shell $(PG_CONFIG) --includedir-server)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# `xpg build` sets BUILD_DIR and expects the library and extension files there
build: $(BUILD_DIR)/$(EXTENSION)$(DLSUFFIX)

$(BUILD_DIR)/.gitignore: $(EXTENSION).control $(DATA)
	mkdir -p $(BUILD_DIR)/extension
	cp $(EXTENSION).control $(DATA) $(BUILD_DIR)/extension
	echo "*" > $(BUILD_DIR)/.gitignore

$(BUILD_DIR)/%.o: src/%.c $(HEADERS) $(BUILD_DIR)/.gitignore
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(EXTENSION)$(DLSUFFIX): $(EXTENSION)$(DLSUFFIX)
	mv $? $@
