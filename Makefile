.DEFAULT_GOAL := help
.DELETE_ON_ERROR:
.PHONY: debug test test-http valgrind clean db-clean db-nuke help

# Debug build (frame pointers kept). Profile it with `samply record
# ./build/dev/quadrature` — see docs/architecture/PROFILING.md.
debug:
	@cmake --preset=dev
	@cmake --build --preset=dev
	@G_MESSAGES_DEBUG=quadrature exec ./build/dev/quadrature

valgrind:
	@cmake --preset=dev
	@cmake --build --preset=dev
	@G_MESSAGES_DEBUG=quadrature G_SLICE=always-malloc G_DEBUG=gc-friendly \
		exec valgrind --leak-check=full --track-origins=yes \
		--suppressions=/usr/share/glib-2.0/valgrind/glib.supp \
		--suppressions=/usr/share/gtk-4.0/valgrind/gtk.supp \
		./build/dev/quadrature

# `.env` is sourced for PG / AcoustID credentials in integration tests.
test:
	@cmake --preset=dev
	@cmake --build --preset=dev
	@if [ -f .env ]; then set -a; . ./.env; set +a; fi; ctest --preset=dev

test-http:
	@cmake --preset=test-http
	@cmake --build --preset=test-http
	@if [ -f .env ]; then set -a; . ./.env; set +a; fi; ctest --preset=test-http

clean:
	@rm -rf build

# Per-library SQLite + album artwork wipe. Preserves fanart.tv artist art by
# default; REMOVE_FANART=1 nukes that too.
REMOVE_FANART ?= 0
db-clean:
	@settings="$$HOME/.config/quadrature/settings.ini"; \
	if [ ! -f "$$settings" ]; then echo "no settings at $$settings"; exit 0; fi; \
	paths=$$(awk '/^\[Library\.[0-9]+\]/{lib_path=""; data_path=""} \
		/^path[[:space:]]*=/{lib_path=$$0; sub(/^[^=]*=[[:space:]]*/, "", lib_path)} \
		/^data_path[[:space:]]*=/{data_path=$$0; sub(/^[^=]*=[[:space:]]*/, "", data_path); \
			if (data_path != "") print data_path; else print lib_path}' "$$settings"); \
	for p in $$paths; do \
		p=$$(echo "$$p" | xargs); [ -z "$$p" ] && continue; \
		rm -f "$$p/quadrature.sqlite" "$$p/quadrature.sqlite-wal" "$$p/quadrature.sqlite-shm"; \
		if [ "$(REMOVE_FANART)" = "1" ]; then \
			rm -rf "$$p/artwork"; \
		else \
			for d in "$$p/artwork"/*/; do \
				case "$$d" in */artists/) continue;; esac; \
				rm -rf "$$d"; \
			done; \
			rm -f "$$p/artwork"/*.atlas; \
		fi; \
	done

db-nuke:
	@$(MAKE) db-clean REMOVE_FANART=1
	@rm -rf "$$HOME/.config/quadrature" "$$HOME/.cache/quadrature"

help:
	@echo "make debug      configure+build dev preset, run with G_MESSAGES_DEBUG"
	@echo "make valgrind   dev build under Valgrind"
	@echo "make test       configure+build dev preset, run ctest"
	@echo "make test-http  build the HTTP-only preset, run ctest serially"
	@echo "make clean      remove build/"
	@echo "make db-clean   wipe SQLite + album art (REMOVE_FANART=1 for fanart too)"
	@echo "make db-nuke    db-clean + remove ~/.config/quadrature and ~/.cache/quadrature"
	@echo ""
	@echo "For arbitrary builds: cmake --build --preset=<dev|release|flatpak|test-http|asan>"
