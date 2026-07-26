.PHONY: all wasm esp32 clean

all: wasm esp32

wasm:
	@$(MAKE) -C wasm build

esp32:
	@source $(HOME)/esp/esp-idf/export.sh >/dev/null 2>&1 && cd firmware && idf.py build

clean:
	@$(MAKE) -C wasm clean
	@source $(HOME)/esp/esp-idf/export.sh >/dev/null 2>&1 && cd firmware && idf.py fullclean
