.PHONY: test test-v debug clean db-clean db-nuke cli production install

BUILD_DIR := build
INSTALL_PREFIX ?= /usr/local

# Run tests (loads .env if present for integration test credentials)
test:
	@[ -f $(BUILD_DIR)/build.ninja ] || cmake -S . -B $(BUILD_DIR) -GNinja -DBUILD_TESTS=ON > /dev/null
	@ninja -C $(BUILD_DIR)
	@if [ -f .env ]; then set -a; . ./.env; set +a; fi; \
	for t in $(BUILD_DIR)/test_*; do [ -x "$$t" ] && "$$t" || exit 1; done

# Development UI build with debug logging
debug:
	@cmake -S . -B $(BUILD_DIR) -GNinja -DBUILD_UI=ON > /dev/null
	@ninja -C $(BUILD_DIR)
	@G_MESSAGES_DEBUG=quadrature exec ./$(BUILD_DIR)/quadrature

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

# Clean build directory
clean:
	@rm -rf $(BUILD_DIR)

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
	paths=$$(awk -F'=' '/^library_paths[[:space:]]*=/{print $$2}' "$$settings" | tr ';' '\n'); \
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
	@echo "  make debug      - Build UI and run with DEBUG logging"
	@echo "  make cli        - Build quadrature-cli (indexer, setup-rt, etc.)"
	@echo "  make production - Release build with UI, CLI, and install files"
	@echo "  make clean      - Remove build directory"
	@echo "  make db-clean   - Remove databases and album artwork (preserves fanart.tv artist art)"
	@echo "  make db-clean REMOVE_FANART=1 - Also remove fanart.tv artist art"
	@echo "  make db-nuke    - Remove all data (databases, artwork, settings, cache)"
