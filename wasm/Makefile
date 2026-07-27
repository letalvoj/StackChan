.PHONY: all build serve clean

# nproc is Linux-only; macOS needs sysctl. Without this the recipe degrades to a
# bare `make -j`, which spawns unbounded parallel jobs.
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

all: build

build:
	@echo "🛠️  [build-wasm] Compiling WASM standalone project via Emscripten..."
	@mkdir -p build_wasm
	@cd build_wasm && emcmake cmake .. && emmake make -j$(NPROC)
	@cp build_wasm/stack-chan.html build_wasm/index.html
	@mkdir -p build_wasm/ui
	@cp ui/* build_wasm/ui/
	@echo "🟢 [build-wasm] ✓ WASM build complete. index.html generated."

serve: build
	@echo "🌐 Starting development web server on port 8081..."
	@python3 serve.py

clean:
	@rm -rf build_wasm
