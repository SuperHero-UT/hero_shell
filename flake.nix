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
        {
          devShells.default = pkgs.mkShell {
            packages = [
              pkgs.cmake
              pkgs.cmake-format
              pkgs.ninja
              pkgs.automake
              pkgs.autoconf
            ];
            shellHook = ''
              unset $NIX_LDFLAGS
            '';
          };

          packages.default = pkgs.stdenv.mkDerivation {
            name = "hero_shell";
            src = ./.;
            buildInputs = [
              pkgs.cmake
              pkgs.ninja
              pkgs.automake
              pkgs.autoconf
            ];
            preConfigure = ''
              unset NIX_LDFLAGS
            '';
            preBuild = ''
              unset NIX_LDFLAGS
            '';
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DCMAKE_IGNORE_PREFIX_PATH=/nix/store"
            ];
          };
        };
    };
}
