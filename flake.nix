{
  description = "Flake shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs =
    inputs@{ flake-parts, nixpkgs, ... }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = nixpkgs.lib.platforms.all;

      perSystem =
        { pkgs, ... }:
        let
          baseBuildInputs =
            (with pkgs; [ cmake ninja automake autoconf ])
            ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isGnu pkgs.glibc.static;
          staticHook = pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isGnu ''
            export LIBRARY_PATH="${pkgs.glibc.static}/lib"''${LIBRARY_PATH:+:$LIBRARY_PATH}
          '';
        in
        {
          devShells.default = pkgs.mkShell {
            packages = baseBuildInputs ++ [ pkgs.cmake-format ];
            shellHook = ''
              unset NIX_LDFLAGS
              unset NIX_CFLAGS_LINK
            ''
            + staticHook;
          };

          packages.default = pkgs.stdenv.mkDerivation {
            name = "hero_shell";
            src = ./.;
            buildInputs = baseBuildInputs;
            preConfigure = ''
              unset NIX_LDFLAGS
              unset NIX_CFLAGS_LINK
            ''
            + staticHook;
            preBuild = ''
              unset NIX_LDFLAGS
              unset NIX_CFLAGS_LINK
            ''
            + staticHook;
            postInstall = ''
              cmake --build build --target package
              mkdir -p $out/dist
              for artifact in build/*.AppImage build/*.appimage build/*.dmg build/*.DMG build/*.tar.gz build/*.tgz; do
                if [ -f "$artifact" ]; then
                  cp "$artifact" $out/dist/
                fi
              done
            '';
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DCMAKE_IGNORE_PREFIX_PATH=/nix/store"
            ];
          };
        };
    };
}
