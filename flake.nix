{
  description = "Qt6/C++ GUI for TRYX display control on NixOS";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs {
        inherit system;
      };
    in
    {
      packages.${system} = rec {
        tryx-panorama-manager = pkgs.callPackage ./package.nix { };
        default = tryx-panorama-manager;
      };

      apps.${system}.default = {
        type = "app";
        program = "${self.packages.${system}.default}/bin/tryx-panorama-manager";
      };

      overlays.default = final: prev: {
        tryx-panorama-manager = final.callPackage ./package.nix { };
      };
    };
}
