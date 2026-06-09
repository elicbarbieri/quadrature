{
  description = "Quadrature - four-channel broadcast audio player";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        coreDeps = with pkgs; [
          ffmpeg-full pipewire sqlite fftw vips rubberband
          chromaprint taglib libpq libsoup_3 json-glib glib-networking
        ];
        buildDeps = with pkgs; [ cmake pkg-config ninja ];
        uiDeps    = with pkgs; [ gtk4 libadwaita gsettings-desktop-schemas glib ];
        devTools  = with pkgs; [ gdb valgrind criterion ];

        mkPackage = preset: pkgs.stdenv.mkDerivation {
          pname = if preset == "release" then "quadrature" else "quadrature-${preset}";
          version = "0.1.0";
          src = ./.;

          # wrapGAppsHook4 wraps the binary so GIO modules, gsettings schemas,
          # icon themes, and gdk-pixbuf loaders resolve outside `nix develop`.
          nativeBuildInputs = buildDeps ++ [ pkgs.wrapGAppsHook4 ];
          # sysprof is a transitive private dep of glib-2.0.
          buildInputs = coreDeps ++ uiDeps ++ [ pkgs.sysprof ];

          configurePhase = ''
            runHook preConfigure
            cmake --preset=${preset} -DCMAKE_INSTALL_PREFIX=$out
            runHook postConfigure
          '';
          buildPhase = ''
            runHook preBuild
            cmake --build --preset=${preset}
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            cmake --install build/${preset}
            runHook postInstall
          '';

          enableParallelBuilding = true;

          meta = with pkgs.lib; {
            description = "Four-channel broadcast audio player with MusicBrainz-enriched library";
            homepage    = "https://github.com/elicbarbieri/quadrature";
            license     = licenses.mit;
            platforms   = platforms.linux;
            mainProgram = "quadrature";
          };
        };

      in {
        packages = {
          default         = self.packages.${system}.quadrature;
          quadrature      = mkPackage "release";
          # HTTP-only build — same compile path the flatpak manifest uses.
          quadrature-http = mkPackage "flatpak";
        };

        devShells.default = pkgs.mkShell {
          name = "quadrature-dev";
          buildInputs = coreDeps ++ buildDeps ++ uiDeps ++ devTools
            ++ [
              pkgs.dconf
              pkgs.libglvnd
              pkgs.vulkan-loader
              pkgs.perf
              pkgs.clang-tools
            ];

          shellHook = ''
            unset NIX_ENFORCE_NO_NATIVE
            export XDG_DATA_DIRS="${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.name}:${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name}:$XDG_DATA_DIRS"
            export GIO_EXTRA_MODULES="${pkgs.glib-networking}/lib/gio/modules:${pkgs.dconf.lib}/lib/gio/modules"
            # NixOS host GL/Vulkan vendor drivers live in /run/opengl-driver.
            # libglvnd / vulkan-loader (in buildInputs) provide the dispatchers;
            # this path lets them find the actual NVIDIA/Mesa ICDs at runtime.
            # libglvnd provides the libEGL.so.1 / libGL.so.1 dispatcher stubs;
            # /run/opengl-driver/lib provides the vendor ICDs (libEGL_nvidia,
            # libGLX_nvidia, etc.) that the dispatcher dlopen()s.
            export LD_LIBRARY_PATH="${pkgs.libglvnd}/lib:${pkgs.vulkan-loader}/lib:/run/opengl-driver/lib:$LD_LIBRARY_PATH"
            # Tell the libglvnd EGL loader where to find vendor ICD JSON
            # descriptors (NixOS puts them under /run/opengl-driver, not /usr).
            export __EGL_VENDOR_LIBRARY_DIRS="/run/opengl-driver/share/glvnd/egl_vendor.d"
          '';
        };

        formatter = pkgs.nixpkgs-fmt;
      });
}
