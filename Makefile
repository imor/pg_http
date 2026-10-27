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
EXTNAME = $(EXTENSION)
EXTVERSION = 0.1

DATA = $(sort $(wildcard sql/*--*.sql) sql/$(EXTNAME)--$(EXTVERSION).sql)

EXTRA_CLEAN = sql/$(EXTNAME)--$(EXTVERSION).sql $(EXTNAME).control
MODULE_big = $(EXTNAME)
HEADERS = $(shell find src -name '*.h')
SOURCES = $(shell find src -name '*.c')

# `xpg build` sets BUILD_DIR; plain `make` falls back to `build` so objects never land in src
BUILD_DIR ?= build
OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(SOURCES))

SHLIB_LINK = -lcurl

PG_CONFIG = pg_config

# treat PostgreSQL headers as system headers so the strict warnings only apply to our code
PG_CPPFLAGS = -Isrc -isystem $(shell $(PG_CONFIG) --includedir-server) -DEXTNAME=\"$(EXTNAME)\" -DEXTVERSION=\"$(EXTVERSION)\"

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# `xpg build` sets BUILD_DIR and expects the library and extension files there
build: $(BUILD_DIR)/$(EXTNAME)$(DLSUFFIX) $(EXTNAME).control $(DATA)

sql/$(EXTNAME)--$(EXTVERSION).sql: sql/$(EXTNAME).sql
	cp $< $@

$(EXTNAME).control: $(EXTNAME).control.in
	sed "s/@EXTNAME@/$(EXTNAME)/g; s/@EXTVERSION@/$(EXTVERSION)/g" $< > $@

$(BUILD_DIR)/.gitignore: $(EXTNAME).control $(DATA)
	mkdir -p $(BUILD_DIR)/extension
	cp $(EXTNAME).control $(DATA) $(BUILD_DIR)/extension
	echo "*" > $(BUILD_DIR)/.gitignore

$(BUILD_DIR)/%.o: src/%.c $(HEADERS) $(BUILD_DIR)/.gitignore
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(EXTNAME)$(DLSUFFIX): $(EXTNAME)$(DLSUFFIX)
	mv $? $@

.PHONY: test
test:
	net-with-nginx python -m pytest -s -vv $(PYTEST_ARGS)
