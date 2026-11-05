# Сборка всех программ конфига и установка в ~/.config/hypr/ (там их ждут скрипты).
#   make install
#
# dashboard, bt-popup, wifi-popup: C++ и FTXUI, собираются через CMake. FTXUI он
# скачает и соберёт сам при первой сборке, ставить его в систему не нужно.
# ws-overview, profile-overview, hidden-apps-view: C++, Cairo и Wayland, см. overview/.

OUTDIR    = $(HOME)/.config/hypr
BUILD_DIR = build
POPUPS    = dashboard bt-popup wifi-popup
OVERVIEWS = ws-overview profile-overview hidden-apps-view

.PHONY: all popups overviews install clean

all: popups overviews

popups:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j

overviews:
	$(MAKE) -C overview

install: all
	install -m755 $(addprefix $(BUILD_DIR)/,$(POPUPS)) $(OUTDIR)/
	install -m755 $(addprefix overview/,$(OVERVIEWS)) $(OUTDIR)/
	@echo "  ✓ установлено в $(OUTDIR)/"

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C overview clean
