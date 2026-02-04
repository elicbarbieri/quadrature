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

          # Set up GSettings schemas for GTK4 file dialogs
          shellHook = ''
            export XDG_DATA_DIRS="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.name}:${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name}:$XDG_DATA_DIRS"
            export GIO_EXTRA_MODULES="${pkgs.dconf.lib}/lib/gio/modules"
          '';
        };

        formatter = pkgs.nixpkgs-fmt;
      }
    );
}
