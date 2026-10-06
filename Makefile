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
test: test-session test-lock test-secure test-secure-dim test-xwindows test-privileged test-sas test-power test-elevated test-popup test-backdrop test-xfloat test-decor test-decorlook test-capture test-wmreq test-jumpclick test-bigwindow test-desktopfront test-selfmove test-deskcomp test-glassframe test-animbg test-embedfocus test-pen

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

# A secure prompt dims the desktop, not black, until the prompt covers it.
.PHONY: test-secure-dim
test-secure-dim: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/secure-dim-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The session's own X11 programs' windows, for Wine's taskbar.
.PHONY: test-xwindows
test-xwindows: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/xwindows-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# A window taken into another (wine-sg's embedding) leaves the keyboard with the one in front.
.PHONY: test-embedfocus
test-embedfocus: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/embedfocus-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# Session-scoped privileged protocols (milestone 3).
test-privileged: build
	@sh test/privileged-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The volume keys: sg-settingsctl steps the volume (with the chime) or mutes.
.PHONY: test-volkeys
test-volkeys: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/volkeys-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

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
# The shell's desktop in front of Linux programs' windows (XDESKTOP), and
# a Linux program bringing its own window forward (_NET_ACTIVE_WINDOW).
.PHONY: test-desktopfront
test-desktopfront: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/desktopfront-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# A Linux program dragged by its own title bar (_NET_WM_MOVERESIZE) moves,
# and stays where it was put.
.PHONY: test-selfmove
test-selfmove: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/selfmove-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

.PHONY: test-wmreq
test-wmreq: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/wmreq-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

.PHONY: test-capture
test-capture: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/capture-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

.PHONY: test-decor
test-decor: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/decor-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

.PHONY: test-jumpclick
test-jumpclick: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/jumpclick-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

.PHONY: test-bigwindow
test-bigwindow: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/bigwindow-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

.PHONY: test-xfloat
test-xfloat: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/xfloat-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# sg-deskcomp: the Wine desktop's compositor -- shadows, real alpha, window
# effects (needs wine-sg with 0744; 77 = a prerequisite missing). And its
# mutants: the gate must fail with each.
test-deskcomp: build
	@sh test/deskcomp-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc
	@sh test/deskcomp-gate.sh --mutants; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The Linux programs' title bars in the Wine frames' look and sizes
# (effects.conf), and a new one held back while the taskbar frames them.
.PHONY: test-decorlook
test-decorlook: build
	@SG_COMPOSITOR_BIN=$(CURDIR)/$(BUILD)/sg-compositor sh test/decorlook-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The Glass look's window frames see-through (needs wine-sg with 0801), and
# the animated background (needs wine-sg with 0804); with their mutants.
test-glassframe: build
	@sh test/glassframe-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc
	@sh test/glassframe-gate.sh --mutants; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

test-animbg: build
	@sh test/animbg-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc
	@sh test/animbg-gate.sh --mutants; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# A pen drives the pointer (a Surface's pen, a drawing tablet): a test build
# (-Dtest-tablet=true, build-pen/) feeds a pen from a FIFO; the mutant build
# (SG_MUTANT_TABLET, build-pen-mutant/) must fail.
.PHONY: test-pen
test-pen:
	@sh test/pen-gate.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || [ $$rc -eq 0 ] || exit $$rc; \
	 sh test/pen-gate.sh --mutant >/dev/null 2>&1; rc=$$?; [ $$rc -eq 1 ] || { echo "pen-gate: the mutant was not caught ($$rc)"; exit 1; }
