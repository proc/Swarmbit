# Convenience wrapper around CMake for Swarmbit.
#
#   make build   - configure + build in build/
#   make test    - run the ctest suite
#   make clean   - remove the build/ directory

BUILD_DIR := build

.PHONY: build test clean

build:
	cmake -S . -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	find $(BUILD_DIR) -mindepth 1 ! -name '.gitkeep' -delete
