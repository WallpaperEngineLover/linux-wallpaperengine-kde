{
  description = "linux-wallpaperengine-kde, Wallpaper Engine live wallpapers on Linux";

  inputs = {
    self.submodules = true;
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      version = "0-unstable-${self.lastModifiedDate or "19700101"}";
      package =
        disableKdeFeatures:
        pkgs.callPackage ./nix/package.nix {
          src = self;
          inherit version disableKdeFeatures;
        };
    in
    {
      packages.${system} = {
        default = package false;
        generic = package true;
      };

      overlays.default = final: prev: {
        linux-wallpaperengine-kde = final.callPackage ./nix/package.nix {
          src = self;
          inherit version;
        };
      };
    };
}
