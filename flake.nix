{
  description = "Quadrature - Professional 4-channel broadcast audio player";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        coreDeps = with pkgs; [
          ffmpeg-full
          pipewire
          sqlite
          fftw
          vips
          rubberband
          chromaprint      # AcoustID fingerprinting
          taglib           # Audio metadata tags
          libpq            # MusicBrainz Postgres queries
          libsoup_3        # HTTP client for fanart.tv
          json-glib        # JSON parsing for fanart.tv responses
          glib-networking  # GnuTLS backend for libsoup3 HTTPS
        ];

        buildDeps = with pkgs; [
          cmake
          pkg-config
          ninja
        ];

        uiDeps = with pkgs; [
          gtk4
          libadwaita
          gsettings-desktop-schemas
          glib
        ];

        devTools = with pkgs; [
          gdb
          valgrind
          criterion
        ];

        mkQuadrature = { pname, buildType, useLibpq ? true }: pkgs.stdenv.mkDerivation {
          inherit pname;
          version = "0.1.0";
          src = ./.;
          # wrapGAppsHook4 wraps the binary so GIO modules (glib-networking
          # for TLS), gsettings schemas, icon themes, and gdk-pixbuf loaders
          # resolve at runtime when launched outside `nix develop`.
          nativeBuildInputs = buildDeps ++ [ pkgs.wrapGAppsHook4 ];
          # sysprof: transitive private dep of glib-2.0 (needed by pkg-config
          # since glib's .pc lists it under Requires.private).
          # glib-networking: GIO TLS modules — wrapGAppsHook4 picks these up.
          # libpq is conditional — useLibpq=false drops it for HTTP-only builds.
          buildInputs = coreDeps ++ uiDeps ++ [ pkgs.glib-networking pkgs.sysprof ]
                        ++ pkgs.lib.optional useLibpq pkgs.libpq;
          cmakeFlags = [
            "-DCMAKE_BUILD_TYPE=${buildType}"
            "-DBUILD_UI=ON"
            "-DBUILD_CLI=ON"
            "-DBUILD_PRODUCTION=ON"
            "-DQUADRATURE_USE_LIBPQ=${if useLibpq then "ON" else "OFF"}"
          ];
          enableParallelBuilding = true;

          meta = with pkgs.lib; {
            description = "Professional 4-channel audio player for broadcast studios";
            license = licenses.mit;
            platforms = platforms.linux;
            mainProgram = "quadrature";
          };
        };

      in
      {
        packages = {
          default = self.packages.${system}.quadrature;
          quadrature       = mkQuadrature { pname = "quadrature";       buildType = "Release"; };
          quadrature-debug = mkQuadrature { pname = "quadrature-debug"; buildType = "Debug";   };
          # HTTP-only build (no libpq) — what the Flatpak manifest produces.
          # Useful for verifying the compile-out path without invoking flatpak-builder.
          quadrature-http  = mkQuadrature { pname = "quadrature-http";  buildType = "Release"; useLibpq = false; };
        };

        devShells.default = pkgs.mkShell {
          name = "quadrature-dev";
          buildInputs = coreDeps ++ buildDeps ++ uiDeps ++ devTools ++ [ pkgs.dconf ];

          shellHook = let
            # Wrap pkg-config to suppress Requires.private "not found" warnings.
            # Nix isolates packages so transitive .pc deps are unfindable, but
            # they're only needed for static linking which we don't use.
            pkgConfigWrapper = pkgs.writeShellScript "pkg-config-quiet" ''
              exec ${pkgs.pkg-config}/bin/pkg-config "$@" 2>/dev/null
            '';
          in ''
            unset NIX_ENFORCE_NO_NATIVE
            export PKG_CONFIG="${pkgConfigWrapper}"
            export XDG_DATA_DIRS="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.name}:${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name}:$XDG_DATA_DIRS"
            export GIO_EXTRA_MODULES="${pkgs.glib-networking}/lib/gio/modules:${pkgs.dconf.lib}/lib/gio/modules"
          '';
        };

        formatter = pkgs.nixpkgs-fmt;
      }
    );
}
