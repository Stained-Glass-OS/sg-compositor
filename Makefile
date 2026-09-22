# sg-compositor — see CLAUDE.md.
BUILD ?= build
SG_SESSION ?= ../sg-session

.PHONY: all build test clean deb install

all: build

$(BUILD)/build.ninja:
	meson setup $(BUILD) --buildtype=release -Dman-pages=disabled

build: $(BUILD)/build.ninja
	ninja -C $(BUILD)

install: build
	DESTDIR=$(DESTDIR) meson install -C $(BUILD) --no-rebuild

# The gate is sg-session's own session gate, run with this compositor hosting
# the session instead of cage. Milestone 1 of ADR 0011 is exactly "that still
# passes", so the gate is the same one, not a new one.
test: build
	@[ -d "$(SG_SESSION)" ] || { echo "sg-session checkout not found at $(SG_SESSION)"; exit 2; }
	$(MAKE) -C $(SG_SESSION) test SG_COMPOSITOR=$(CURDIR)/$(BUILD)/sg-compositor

deb:
	dpkg-buildpackage -us -uc -b

clean:
	rm -rf $(BUILD)
