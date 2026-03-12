{
  description = "Quadrature - Professional 4-channel broadcast audio player";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs {
          inherit system;
          config.allowUnfree = true;
        };

        performanceFlags = [
          "-O3"
          "-march=native"
          "-mtune=native"
          "-ffast-math"
          "-fno-finite-math-only"
          "-fno-trapping-math"
          "-funroll-loops"
          "-fomit-frame-pointer"
          "-flto"
          "-fuse-linker-plugin"
          "-DNDEBUG"
        ];

        coreDeps = with pkgs; [
          ffmpeg-full
          alsa-lib
          pipewire
          libatomic_ops
          sqlite
          fftw
          vips
          rubberband
          libmusicbrainz
          chromaprint      # AcoustID fingerprinting
          taglib           # Read metadata tags from audio files
          postgresql       # libpq for MusicBrainz PG queries
          openssl          # libssl/libcrypto (transitive: libpq)
          libsysprof-capture  # sysprof-capture-4 (transitive: glib-2.0)
          libsoup_3        # HTTP client for fanart.tv artist art
          json-glib         # JSON parsing for fanart.tv API responses
          glib-networking   # GnuTLS TLS backend for GIO (required by libsoup3 for HTTPS)
        ];

        buildDeps = with pkgs; [
          cmake
          pkg-config
          ninja
          gcc13
        ];

        uiDeps = with pkgs; [
          gtk4
          gsettings-desktop-schemas
          glib
        ];

        devTools = with pkgs; [
          gdb
          valgrind
          cppcheck
          clang-tools
          criterion
          boxfort
        ];

      in
      {
        packages = {
          default = self.packages.${system}.quadrature;

          quadrature = pkgs.stdenv.mkDerivation {
            pname = "quadrature";
            version = "0.1.0";
            src = ./.;
            # Allow -march=native for audio DSP performance
            env.NIX_ENFORCE_NO_NATIVE = "";
            nativeBuildInputs = buildDeps;
            buildInputs = coreDeps ++ uiDeps;
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DBUILD_TESTS=ON"
              "-DBUILD_EXAMPLES=ON"
              "-GNinja"
            ];
            NIX_CFLAGS_COMPILE = builtins.concatStringsSep " " performanceFlags;
            NIX_LDFLAGS = "-flto -fuse-linker-plugin";
            enableParallelBuilding = true;

            meta = with pkgs.lib; {
              description = "Professional 4-channel audio player for broadcast studios";
              license = licenses.mit;
              platforms = platforms.linux;
            };
          };

          quadrature-debug = pkgs.stdenv.mkDerivation {
            pname = "quadrature-debug";
            version = "0.1.0";
            src = ./.;
            nativeBuildInputs = buildDeps;
            buildInputs = coreDeps ++ uiDeps;
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Debug"
              "-DBUILD_TESTS=ON"
              "-DBUILD_EXAMPLES=ON"
              "-GNinja"
            ];
            NIX_CFLAGS_COMPILE = "-O0 -g -fsanitize=address,undefined";
            NIX_LDFLAGS = "-fsanitize=address,undefined";
            enableParallelBuilding = true;
          };
        };

        devShells.default = pkgs.mkShell {
          name = "quadrature-dev";
          buildInputs = coreDeps ++ buildDeps ++ uiDeps ++ devTools ++ [
            pkgs.dconf
          ];

          shellHook = let
            # Wrap pkg-config to suppress Requires.private "not found" warnings.
            # Nix isolates packages so transitive .pc deps are unfindable, but
            # they're only needed for static linking which we don't use. Real
            # errors (missing direct deps) still surface through CMake itself.
            pkgConfigWrapper = pkgs.writeShellScript "pkg-config-quiet" ''
              exec ${pkgs.pkg-config}/bin/pkg-config "$@" 2>/dev/null
            '';
          in ''
            # Allow -march=native for local dev builds (high-perf audio DSP)
            unset NIX_ENFORCE_NO_NATIVE

            export PKG_CONFIG="${pkgConfigWrapper}"

            # GSettings schemas for GTK4 file dialogs
            export XDG_DATA_DIRS="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.name}:${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name}:$XDG_DATA_DIRS"
            export GIO_EXTRA_MODULES="${pkgs.glib-networking}/lib/gio/modules:${pkgs.dconf.lib}/lib/gio/modules"
          '';
        };

        formatter = pkgs.nixpkgs-fmt;
      }
    );
}
