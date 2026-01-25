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
            name = "hello";
            src = ./.;
            buildInputs = [ ];
            installPhase = '''';
          };
        };
    };
}
