# WUP ThinClient — root build entry point.
#
#   make setup     fetch the pinned SDK + LLVM toolchain, validate
#   make           build build/apps/wup_thinclient.wup
#   make -j8       same (parallel where the SDK supports it)
#   make test      run host-side unit tests
#   make clean     remove generated artifacts
#   make rebuild   clean + build
#   make deploy    build if needed, copy .wup to the NetFS share
#   make help      this text
#
# Overrides: LIBWUP_DIR=/path/to/libwup  LLVM_BIN=/path/to/llvm/bin
#            WUP_SHARE_DIR=/path/to/share (default: $HOME/wup-share)

APP       := wup_thinclient
.DEFAULT_GOAL := build
WUP       := build/apps/$(APP).wup
SCRIPTS   := scripts

.PHONY: help setup build test clean rebuild deploy

help:
	@echo "WUP ThinClient — available targets:"
	@echo "  setup    prepare pinned libwup SDK + LLVM toolchain"
	@echo "  build    compile $(WUP)"
	@echo "  test     run host-side unit tests"
	@echo "  clean    remove build artifacts"
	@echo "  rebuild  clean + build"
	@echo "  deploy   copy the .wup to \$WUP_SHARE_DIR (default ~/wup-share)"
	@echo ""
	@echo "Env: LIBWUP_DIR, LLVM_BIN, WUP_SHARE_DIR"

setup:
	@bash $(SCRIPTS)/setup.sh

build/env.sh: setup

build: build/env.sh
	@bash $(SCRIPTS)/build.sh

test:
	@mkdir -p build/tests
	@cc -std=c11 -Wall -Wextra -Werror -Ipad pad/ip_editor.c tests/test_ip_editor.c -o build/tests/test_ip_editor
	@build/tests/test_ip_editor
	@python3 -m unittest discover -s tests -v

clean:
	rm -rf build

rebuild: clean build

deploy: build
	@bash $(SCRIPTS)/deploy.sh
