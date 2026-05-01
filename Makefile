.PHONY: test test-v test-http debug valgrind clean db-clean db-nuke cli production install

BUILD_DIR := build
BUILD_DIR_HTTP := build-http
INSTALL_PREFIX ?= /usr/local

# Run tests (loads .env if present for integration test credentials)
test:
	@[ -f $(BUILD_DIR)/build.ninja ] || cmake -S . -B $(BUILD_DIR) -GNinja -DBUILD_TESTS=ON > /dev/null
	@ninja -C $(BUILD_DIR)
	@if [ -f .env ]; then set -a; . ./.env; set +a; fi; \
	for t in $(BUILD_DIR)/test_*; do [ -x "$$t" ] && "$$t" || exit 1; done

# Run all integration + E2E tests against the public MusicBrainz/AcoustID
# HTTP APIs instead of the self-hosted PostgreSQL stack. Designed to surface
# bugs in the HTTP backend (JSON parsing, rate limiting, error handling).
#
# Slow by design: rate limits are 1 req/sec to musicbrainz.org and ~3 req/sec
# to api.acoustid.org. Expect 5-15 minutes for the full suite.
#
# No env vars required. The HTTP backend uses the bundled AcoustID
# application key (ACOUSTID_APPLICATION_KEY, set in CMakeLists.txt).
#
# Tests using direct mb_pg_* calls (test_mb_resolve) skip automatically —
# they exercise the libpq client itself and have no HTTP analog.
#
# --jobs=1 is REQUIRED: Criterion's default parallel mode forks N test
# workers, each with its own (process-local) rate limiter state. N workers
# × 0.91 req/sec each = N req/sec aggregate against MB's published 1 req/sec
# ceiling → 503s, socket timeouts, false test failures. Serial execution
# keeps the in-process limiter honest. Slow by design.
test-http:
	@[ -f $(BUILD_DIR_HTTP)/build.ninja ] || cmake -S . -B $(BUILD_DIR_HTTP) -GNinja \
		-DBUILD_TESTS=ON > /dev/null
	@ninja -C $(BUILD_DIR_HTTP)
	@if [ -f .env ]; then set -a; . ./.env; set +a; fi; \
	export QUADRATURE_TEST_HTTP=1; \
	echo "═══════════════════════════════════════════════════════════════"; \
	echo "  Running integration tests in HTTP mode (public APIs)"; \
	echo "  Rate limits: ≤0.91 req/sec MB, ≤2.86 req/sec AcoustID"; \
	echo "  Serial (--jobs=1) to keep the in-process limiter honest."; \
	echo "  Expect 5–15 minutes for the full suite."; \
	echo "═══════════════════════════════════════════════════════════════"; \
	for t in $(BUILD_DIR_HTTP)/test_*; do [ -x "$$t" ] && "$$t" --jobs 1 || exit 1; done

# Development UI build with debug logging
debug:
	@cmake -S . -B $(BUILD_DIR) -GNinja -DBUILD_UI=ON > /dev/null
	@ninja -C $(BUILD_DIR)
	@G_MESSAGES_DEBUG=quadrature exec ./$(BUILD_DIR)/quadrature

# Debug build under Valgrind (leak checking + origin tracking)
valgrind:
	@cmake -S . -B $(BUILD_DIR) -GNinja -DBUILD_UI=ON -DCMAKE_BUILD_TYPE=Debug > /dev/null
	@ninja -C $(BUILD_DIR)
	@G_MESSAGES_DEBUG=quadrature G_SLICE=always-malloc G_DEBUG=gc-friendly \
		exec valgrind --leak-check=full --track-origins=yes \
		--suppressions=/usr/share/glib-2.0/valgrind/glib.supp \
		--suppressions=/usr/share/gtk-4.0/valgrind/gtk.supp \
		./$(BUILD_DIR)/quadrature

# Build quadrature-cli (indexer, setup-rt, etc.)
cli:
	@cmake -S . -B $(BUILD_DIR) -GNinja -DBUILD_CLI=ON > /dev/null
	@ninja -C $(BUILD_DIR)
	@echo "Built: $(BUILD_DIR)/quadrature-cli"

# Production build (UI + daemon + install targets)
production:
	@cmake -S . -B $(BUILD_DIR) -GNinja \
		-DBUILD_UI=ON \
		-DBUILD_CLI=ON \
		-DBUILD_PRODUCTION=ON \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(INSTALL_PREFIX) > /dev/null
	@ninja -C $(BUILD_DIR)
	@echo "Production build complete"
	@echo "  UI:   $(BUILD_DIR)/quadrature"
	@echo "  CLI:  $(BUILD_DIR)/quadrature-cli"
	@echo "Run 'make install' to install"

# Clean build directories (PG mode + HTTP mode)
clean:
	@rm -rf $(BUILD_DIR) $(BUILD_DIR_HTTP)

# Clean per-library SQLite databases and album artwork (preserves artist art from fanart.tv)
# Use REMOVE_FANART=1 to also delete artist artwork: make db-clean REMOVE_FANART=1
REMOVE_FANART ?= 0
db-clean:
	@echo "Removing quadrature library databases and album artwork..."
	@if [ "$(REMOVE_FANART)" = "1" ]; then echo "  (including fanart.tv artist art)"; fi
	@settings="$$HOME/.config/quadrature/settings.ini"; \
	if [ ! -f "$$settings" ]; then \
		echo "  No settings found at $$settings"; exit 0; \
	fi; \
	paths=$$(awk '/^\[Library\.[0-9]+\]/{lib_path=""; data_path=""} \
		/^path[[:space:]]*=/{lib_path=$$0; sub(/^[^=]*=[[:space:]]*/, "", lib_path)} \
		/^data_path[[:space:]]*=/{data_path=$$0; sub(/^[^=]*=[[:space:]]*/, "", data_path); \
			if (data_path != "") print data_path; else print lib_path}' "$$settings"); \
	for p in $$paths; do \
		p=$$(echo "$$p" | xargs); \
		[ -z "$$p" ] && continue; \
		for f in "$$p/quadrature.sqlite" "$$p/quadrature.sqlite-wal" "$$p/quadrature.sqlite-shm"; do \
			[ -f "$$f" ] && rm "$$f" && echo "  removed $$f"; \
		done; \
		if [ "$(REMOVE_FANART)" = "1" ]; then \
			[ -d "$$p/artwork" ] && rm -rf "$$p/artwork" && echo "  removed $$p/artwork"; \
		else \
			for d in "$$p/artwork"/*/; do \
				case "$$d" in */artists/) continue;; esac; \
				[ -d "$$d" ] && rm -rf "$$d" && echo "  removed $$d"; \
			done; \
			for f in "$$p/artwork"/*.atlas; do \
				[ -f "$$f" ] && rm "$$f" && echo "  removed $$f"; \
			done; \
		fi; \
	done
	@echo "Done."

# Nuclear option - remove ALL quadrature data (databases, artwork, settings, cache)
db-nuke:
	@echo "Removing ALL quadrature data (databases, artwork, settings, cache)..."
	@$(MAKE) db-clean REMOVE_FANART=1
	@rm -rf "$$HOME/.config/quadrature"
	@rm -rf "$$HOME/.cache/quadrature"
	@echo "Done."

# Help
help:
	@echo "Quadrature Build Targets:"
	@echo ""
	@echo "  make test       - Run unit tests (loads .env for integration test credentials)"
	@echo "  make test-http  - Run integration/E2E tests against the public MB+AcoustID APIs"
	@echo "                    (slow, 5-15min; requires ACOUSTID_API_KEY for fingerprint tests)"
	@echo "  make debug      - Build UI and run with DEBUG logging"
	@echo "  make valgrind   - Debug build under Valgrind (leak check + origin tracking)"
	@echo "  make cli        - Build quadrature-cli (indexer, setup-rt, etc.)"
	@echo "  make production - Release build with UI, CLI, and install files"
	@echo "  make clean      - Remove build directory"
	@echo "  make db-clean   - Remove databases and album artwork (preserves fanart.tv artist art)"
	@echo "  make db-clean REMOVE_FANART=1 - Also remove fanart.tv artist art"
	@echo "  make db-nuke    - Remove all data (databases, artwork, settings, cache)"
