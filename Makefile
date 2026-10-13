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
DATA = sql/pg_http--0.1.sql
MODULES = src/pg_http
SHLIB_LINK = -lcurl

PG_CONFIG = pg_config

# treat PostgreSQL headers as system headers so the strict warnings only apply to our code
PG_CPPFLAGS = -isystem $(shell $(PG_CONFIG) --includedir-server)

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# `xpg build` sets BUILD_DIR and expects the library and extension files there
build: all
	mkdir -p $(BUILD_DIR)/extension
	mv src/pg_http$(DLSUFFIX) $(BUILD_DIR)
	cp pg_http.control sql/pg_http--0.1.sql $(BUILD_DIR)/extension
	echo "*" > $(BUILD_DIR)/.gitignore
