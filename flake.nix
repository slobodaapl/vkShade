{
  description = "vkShade — Vulkan post-processing layer with in-game UI (Wayland + X11)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    git-hooks = {
      url = "github:cachix/git-hooks.nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      git-hooks,
    }:
    let
      supportedSystems = [ "x86_64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { localSystem.system = system; };
        in
        {
          default = self.packages.${system}.vkshade;

          vkshade = pkgs.stdenv.mkDerivation {
            pname = "vkshade";
            version = "0.1.0-unstable-${self.shortRev or "dirty"}";

            src = self;

            nativeBuildInputs = with pkgs; [
              meson
              ninja
              pkg-config
              glslang
              util-linux
              wayland-scanner
              pkgsCross.mingwW64.stdenv.cc
            ];

            buildInputs = with pkgs; [
              vulkan-headers
              vulkan-loader
              spirv-headers
              libx11
              libxi
              wayland
              wayland-protocols
              libxkbcommon
            ];

            mesonBuildType = "release";

            mesonFlags = [
              "-Dappend_libdir_vkshade=false"
              "-Dproton_bridge=enabled"
              "--sysconfdir=/etc"
            ];

            postInstall = ''
              # vkshade wrapper: sets ENABLE_VKSHADE and LD_AUDIT for Wine
              # Wayland input interposition (dlopen RTLD_LOCAL bypass).
              mkdir -p "$out/bin"
              substitute ${./scripts/vkshade.sh} "$out/bin/vkshade" \
                --subst-var out
              chmod +x "$out/bin/vkshade"
            '';

            meta = with pkgs.lib; {
              description = "Vulkan post-processing layer with real-time overlay UI (Wayland + X11)";
              license = with licenses; [
                zlib
                bsd2
                mit
              ];
              platforms = [ "x86_64-linux" ];
              mainProgram = "vkshade";
            };
          };

          vkshade-debug = self.packages.${system}.vkshade.overrideAttrs {
            mesonBuildType = "debug";
          };
        }
      );

      formatter = forAllSystems (system: (import nixpkgs { localSystem.system = system; }).nixfmt);

      checks = forAllSystems (system: {
        pre-commit = git-hooks.lib.${system}.run {
          src = self;
          hooks = {
            nixfmt.enable = true;
          };
        };
      });

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { localSystem.system = system; };
        in
        {
          default = pkgs.mkShell {
            inherit (self.checks.${system}.pre-commit) shellHook;
            inputsFrom = [ self.packages.${system}.vkshade ];
            buildInputs =
              self.checks.${system}.pre-commit.enabledPackages
              ++ (with pkgs; [
                actionlint
                clang-tools
                git
                jq
                just
                nil
                nixfmt
                nodejs
                python3
                uv
              ]);
          };
        }
      );

      overlays.default = final: _prev: {
        inherit (self.packages.${final.stdenv.hostPlatform.system}) vkshade;
      };
    };
}
