# sg-compositor — see CLAUDE.md.
BUILD ?= build
SG_SESSION ?= ../sg-session

.PHONY: all build test test-session test-lock test-privileged test-sas test-power test-elevated test-popup test-backdrop clean deb install

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
test: test-session test-lock test-secure test-privileged test-sas test-power test-elevated test-popup test-backdrop

test-session: build
	@[ -d "$(SG_SESSION)" ] || { echo "sg-session checkout not found at $(SG_SESSION)"; exit 2; }
	$(MAKE) -C $(SG_SESSION) test SG_COMPOSITOR=$(CURDIR)/$(BUILD)/sg-compositor

# Lock-mode input isolation (milestone 2). Needs sg-session's adversary fixture,
# built on demand; 77 means a prerequisite is missing and is not a failure.
test-lock: build
	@$(MAKE) -C $(SG_SESSION) security >/dev/null
	@SG_SESSION=$(SG_SESSION) sh test/lock-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

deb:
	dpkg-buildpackage -us -uc -b

clean:
	rm -rf $(BUILD)

# Secure prompt (elevation consent, ADR 0012): isolation without a lock screen.
test-secure: build
	@$(MAKE) -C $(SG_SESSION) security >/dev/null
	@SG_SESSION=$(SG_SESSION) sh test/secure-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# Session-scoped privileged protocols (milestone 3).
test-privileged: build
	@sh test/privileged-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# Reserved keys: Win+L and Ctrl+Alt+Del.
test-sas: build
	@sh test/sas-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# Display power and idle (wlopm, swayidle): Settings' "Turn off the screen after".
test-power: build
	@sh test/power-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# Elevated programs' displays (ADR 0012, bug B56): an elevated program's own X
# server, managed by this compositor; a session program can neither drive nor
# read it. Needs sg-session's sg-elevated-run and sg-vkbd, and sudo -n to run
# as another account (SG_ELEVATED_USER, default sgsystem); 77 = skipped.
test-elevated: build
	@$(MAKE) -C $(SG_SESSION) procagent vkbd >/dev/null
	@SG_SESSION=$(SG_SESSION) sh test/elevated-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# Override-redirect windows: Setup's combo-box lists open, stay open, and a
# click after one closes does not crash the compositor. Needs wine-sg and
# mingw; 77 = skipped.
test-popup: build
	@sh test/popup-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The backdrop: where no window is, colour and picture, not black.
test-backdrop: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/backdrop-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# An X11 window of a Linux program keeps the size it asks for, centred.
.PHONY: test-xfloat
test-xfloat: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/xfloat-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc
