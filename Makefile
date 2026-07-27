# Both targets are built by `make all` on purpose: the WASM harness and the ESP32
# firmware share the same C++ sources, so a change that satisfies one toolchain can
# easily break the other. Building only one hides that until much later.

# export.sh is a bash/zsh script and uses `source`; /bin/sh is dash on most Linux
# distros, which would fail here.
SHELL := /bin/bash

# Resolve the ESP-IDF checkout, in order of preference:
#   1. IDF_PATH from the environment (an already-activated or user-configured IDF)
#   2. the sibling checkout next to this repo
#   3. the conventional ~/esp/esp-idf location
# Hardcoding a single absolute path makes the repo unbuildable on any other machine.
REPO_ROOT := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
IDF_CANDIDATES := $(IDF_PATH) $(REPO_ROOT)/../esp-idf $(HOME)/esp/esp-idf
IDF_PATH_RESOLVED := $(firstword $(foreach d,$(IDF_CANDIDATES),$(wildcard $(d)/export.sh)))

.PHONY: all wasm esp32 clean check-idf

all: wasm esp32

check-idf:
ifeq ($(IDF_PATH_RESOLVED),)
	@echo "error: no ESP-IDF checkout found (looked for export.sh in):" >&2
	@$(foreach d,$(IDF_CANDIDATES),echo "  - $(d)" >&2;)
	@echo "Set IDF_PATH=/path/to/esp-idf and retry." >&2
	@exit 1
endif

wasm:
	@$(MAKE) -C wasm build

# Errors from export.sh are deliberately NOT redirected to /dev/null: a failed
# activation used to surface only as a confusing "idf.py: command not found".
esp32: check-idf
	@source "$(IDF_PATH_RESOLVED)" >/dev/null && cd firmware && idf.py build

clean: check-idf
	@$(MAKE) -C wasm clean
	@source "$(IDF_PATH_RESOLVED)" >/dev/null && cd firmware && idf.py fullclean
