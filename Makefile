EXTENSION = pg_http
DATA = sql/pg_http--0.1.sql
MODULES = src/pg_http
SHLIB_LINK = -lcurl

PG_CONFIG = pg_config

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# `xpg build` sets BUILD_DIR and expects the library and extension files there
build: all
	mkdir -p $(BUILD_DIR)/extension
	mv src/pg_http.dylib $(BUILD_DIR)
	cp pg_http.control sql/pg_http--0.1.sql $(BUILD_DIR)/extension
	echo "*" > $(BUILD_DIR)/.gitignore
