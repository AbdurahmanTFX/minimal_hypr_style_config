# Makefile — profile-overview / ws-overview (C++ + Cairo + wlr-layer-shell)
#
# Требуются пакеты (Arch):   wayland wayland-protocols wlr-protocols libxkbcommon cairo
# Требуются пакеты (Debian): libwayland-dev wayland-protocols libxkbcommon-dev libcairo2-dev
#   (wlr-protocols в Debian/Ubuntu обычно ставится отдельно, см. ниже)
#
# wlr-layer-shell-unstable-v1.xml не входит в основной wayland-protocols,
# а поставляется пакетом wlr-protocols (или идёт вместе с исходниками
# wlroots/Hyprland). Makefile сам ищет xml по стандартным путям; если не
# нашёл — положите файл рядом и укажите WLR_PROTO_XML=путь при вызове make.

CC		 := gcc
CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter
PKGS     := wayland-client xkbcommon cairo
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

GEN_DIR  := gen
COMMON_DIR := common
SRC_DIR  := src

WAYLAND_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner 2>/dev/null)
ifeq ($(WAYLAND_SCANNER),)
    WAYLAND_SCANNER := wayland-scanner
endif

# Поиск wlr-layer-shell-unstable-v1.xml по типичным путям разных дистрибутивов.
WLR_PROTO_XML ?= $(firstword $(wildcard \
    /usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
    /usr/local/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml \
    $(shell pkg-config --variable=pkgdatadir wlr-protocols 2>/dev/null)/unstable/wlr-layer-shell-unstable-v1.xml \
    protocols/wlr-layer-shell-unstable-v1.xml))

PROTO_HDR := $(GEN_DIR)/wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_SRC := $(GEN_DIR)/wlr-layer-shell-unstable-v1-protocol.c
XDG_XML := /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
XDG_HDR := $(GEN_DIR)/xdg-shell-client-protocol.h
XDG_SRC := $(GEN_DIR)/xdg-shell-protocol.c

COMMON_OBJS := \
	$(COMMON_DIR)/layer_window.o \
	$(GEN_DIR)/wlr-layer-shell-unstable-v1-protocol.o \
	$(GEN_DIR)/xdg-shell-protocol.o

.PHONY: all clean check-proto
all: check-proto profile-overview ws-overview hidden-apps-view

check-proto:
	@if [ -z "$(WLR_PROTO_XML)" ]; then \
	  echo "Не найден wlr-layer-shell-unstable-v1.xml."; \
	  echo "  Установите пакет wlr-protocols (Arch: pacman -S wlr-protocols;"; \
	  echo "  из исходников: https://gitlab.freedesktop.org/wlroots/wlr-protocols)"; \
	  echo "  либо положите файл в ./protocols/ и запустите make снова,"; \
	  echo "  либо укажите путь явно:"; \
	  echo "    make WLR_PROTO_XML=/path/to/wlr-layer-shell-unstable-v1.xml"; \
	  exit 1; \
	fi

$(GEN_DIR):
	mkdir -p $(GEN_DIR)

$(PROTO_HDR): $(WLR_PROTO_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $(WLR_PROTO_XML) $(PROTO_HDR)
	sed -i 's/const char \*namespace)/const char *namespace_)/g; s/, namespace);/, namespace_);/g' $(PROTO_HDR)

$(PROTO_SRC): $(WLR_PROTO_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $(WLR_PROTO_XML) $(PROTO_SRC)

$(XDG_HDR): $(XDG_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) client-header $(XDG_XML) $(XDG_HDR)

$(XDG_SRC): $(XDG_XML) | $(GEN_DIR)
	$(WAYLAND_SCANNER) private-code $(XDG_XML) $(XDG_SRC)

$(GEN_DIR)/xdg-shell-protocol.o: $(XDG_SRC) $(XDG_HDR)
	$(CC) $(PKG_CFLAGS) -I$(GEN_DIR) -c $(XDG_SRC) -o $@

$(GEN_DIR)/wlr-layer-shell-unstable-v1-protocol.o: $(PROTO_SRC) $(PROTO_HDR)
	$(CC) $(PKG_CFLAGS) -I$(GEN_DIR) -c $(PROTO_SRC) -o $@

$(COMMON_DIR)/layer_window.o: $(COMMON_DIR)/layer_window.cpp $(COMMON_DIR)/layer_window.hpp $(PROTO_HDR) $(XDG_HDR)
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -I$(GEN_DIR) -c $< -o $@

profile-overview: $(SRC_DIR)/profile_overview.cpp $(COMMON_OBJS) $(PROTO_HDR) ../config.h common/proc_helpers.hpp
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -I$(GEN_DIR) $< $(COMMON_OBJS) -o $@ $(PKG_LIBS)

ws-overview: $(SRC_DIR)/ws_overview.cpp $(COMMON_OBJS) $(PROTO_HDR) ../config.h common/proc_helpers.hpp
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -I$(GEN_DIR) $< $(COMMON_OBJS) -o $@ $(PKG_LIBS)

hidden-apps-view: $(SRC_DIR)/hidden_apps_view.cpp $(COMMON_OBJS) $(PROTO_HDR) ../config.h common/proc_helpers.hpp
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -I$(GEN_DIR) $< $(COMMON_OBJS) -o $@ $(PKG_LIBS)

clean:
	rm -rf $(GEN_DIR) $(COMMON_DIR)/*.o profile-overview ws-overview hidden-apps-view
