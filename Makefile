BOARD = nice_nano//zmk
SHIELD_SIDES = left right
MODULES = . $(wildcard ./modules/*)
__SPACE = $(eval) $(eval)
ZMK_EXTRA_MODULES = $(subst $(__SPACE),;,$(realpath $(MODULES)))

note:
	echo "Usage: make <target>"
	echo
	echo "Targets:"
	echo
	echo "  init: initialize a zmk environment with uv"
	echo
	echo "  install-sdk: install zephyr sdk (arm-zephyr-eabi)"
	echo
	echo "  clean-build: pristine build, cleaning things before building"
	echo
	echo "  build: build the firmware"

init:
	cd zmk && west init -l app/
	cd zmk && west update -o=--depth=1 -n
	cd zmk && west zephyr-export
	cd zmk && west packages pip | xargs uv pip install

install-sdk:
	# Until zmk fork incorporates the fix:
	# https://github.com/zephyrproject-rtos/zephyr/issues/113746
	patch zmk/zephyr/cmake/modules/FindZephyr-sdk.cmake scripts/zephyr-issue-113746.patch
	cd zmk && west sdk install --install-dir="$(shell pwd)/.sdk" -t arm-zephyr-eabi

resources:
	python scripts/gen_material_icons.py modules/zmk-driver-jd9613/src/widgets/icons

$(SHIELD_SIDES): %:
	cd zmk/app && west build $(BUILD_FLAGS) -b "$(BOARD)" -S zmk-usb-logging -- \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
		-DSHIELD=keypaw48_$* \
		-DZMK_EXTRA_MODULES="$(ZMK_EXTRA_MODULES)" \
		-DZMK_CONFIG="$(realpath config)"
	mkdir -p build
	mv zmk/app/build/zephyr/zmk.uf2 build/zmk_$*.uf2

$(SHIELD_SIDES:=-clean-build): BUILD_FLAGS := -p
%-clean-build: %
	@:
clean-build: $(SHIELD_SIDES:=-clean-build)
	@:

build: $(SHIELD_SIDES)
	echo "Built firmware should be at zmk/app/build/zephyr/zmk_*.uf2"

.PHONY: init install-sdk clean-build build $(SHIELD_SIDES)
