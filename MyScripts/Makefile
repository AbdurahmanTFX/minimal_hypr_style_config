CC       = gcc
CFLAGS   = -O2 -Wall
LDFLAGS_GTK4    = $(shell pkg-config --cflags --libs gtk4) -lm
OUTDIR   = $(HOME)/.config/hypr
BUILD_DIR = build

TARGETS = dashboard bt-popup wifi-popup ws-overview profile-overview

.PHONY: all install clean

all: $(TARGETS)

# dashboard, bt-popup и wifi-popup теперь написаны на C++ / FTXUI (см.
# dashboard.cpp, bt-popup.cpp, wifi-popup.cpp). Собираются через CMake — он
# сам скачает и соберёт FTXUI при первой сборке (см. CMakeLists.txt),
# ничего ставить в систему не нужно. ws-overview и profile-overview
# остались на GTK4/Cairo (это графические окна, а не терминальные попапы,
# так что FTXUI для них не подходит) и по-прежнему собираются напрямую gcc.
# "&:" — все три файла производятся одним запуском рецепта (GNU Make >=
# 4.3; на более старом Make пересборка просто чуть менее эффективна, но
# корректна).
dashboard bt-popup wifi-popup &: dashboard.cpp bt-popup.cpp wifi-popup.cpp CMakeLists.txt
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD_DIR) -j
	cp $(BUILD_DIR)/dashboard $(BUILD_DIR)/bt-popup $(BUILD_DIR)/wifi-popup .
	@echo "  ✓ dashboard"
	@echo "  ✓ bt-popup"
	@echo "  ✓ wifi-popup"

ws-overview: ws-overview.c config.h
	$(CC) $(CFLAGS) ws-overview.c -o ws-overview $(LDFLAGS_GTK4)
	@echo "  ✓ ws-overview"

profile-overview: profile-overview.c config.h
	$(CC) $(CFLAGS) profile-overview.c -o profile-overview $(LDFLAGS_GTK4)
	@echo "  ✓ profile-overview"

install: all
	install -m755 dashboard     $(OUTDIR)/
	install -m755 bt-popup      $(OUTDIR)/
	install -m755 wifi-popup    $(OUTDIR)/
	install -m755 ws-overview   $(OUTDIR)/
	install -m755 profile-overview $(OUTDIR)/
	@echo "  ✓ установлено в $(OUTDIR)/"

clean:
	rm -rf $(TARGETS) $(BUILD_DIR)
