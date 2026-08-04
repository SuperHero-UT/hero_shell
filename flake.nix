{
  description = "Flake shell";

  inputs = {
    self.submodules = true;
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
  };

  outputs =
    inputs@{
      self,
      flake-parts,
      nixpkgs,
      ...
    }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      systems = nixpkgs.lib.platforms.all;

      perSystem =
        { pkgs, ... }:
        let
          baseBuildInputs =
            (with pkgs; [
              cmake
              ninja
              automake
              autoconf
              root
              python3
            ])
            ++ pkgs.lib.optional pkgs.stdenv.hostPlatform.isGnu pkgs.glibc.static;
          staticHook = pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isGnu ''
            export LIBRARY_PATH="${pkgs.glibc.static}/lib"''${LIBRARY_PATH:+:$LIBRARY_PATH}
          '';
        in
        {
          devShells.default = pkgs.mkShell {
            packages = baseBuildInputs ++ [
              pkgs.cmake-format
              pkgs.python3
            ];
            shellHook = ''
              unset NIX_LDFLAGS
              unset NIX_CFLAGS_LINK
            ''
            + staticHook;
          };

          packages.default = pkgs.stdenv.mkDerivation {
            name = "hero_shell";
            src = self;
            nativeBuildInputs = [ pkgs.makeWrapper ];
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
            cmakeFlags = [
              "-DCMAKE_BUILD_TYPE=Release"
              "-DCMAKE_IGNORE_PREFIX_PATH=/nix/store"
            ];
            postFixup = ''
              wrapProgram "$out/bin/hero_shell" \
                --prefix PATH : "$out/bin:${pkgs.lib.makeBinPath [ pkgs.python3 ]}"
            '';
          };
        };
    };
}
