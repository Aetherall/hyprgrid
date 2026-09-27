{
  description = "hyprgrid: a 2D workspace grid for Hyprland";

  inputs = {
    hyprland.url = "github:hyprwm/Hyprland";
    nixpkgs.follows = "hyprland/nixpkgs";
    systems.follows = "hyprland/systems";
  };

  outputs = {
    self,
    hyprland,
    nixpkgs,
    systems,
    ...
  }: let
    inherit (nixpkgs) lib;
    eachSystem = lib.genAttrs (import systems);
    version = self.shortRev or self.dirtyShortRev or "unknown";

    # A plugin must be built with the headers and compiler of the Hyprland that
    # loads it, so the build takes that Hyprland package as an argument.
    mkHyprgrid = pkgs: hyprlandPackage:
      hyprlandPackage.stdenv.mkDerivation {
        pname = "hyprgrid";
        inherit version;
        # Without local build output: a `path:` input copies untracked files too.
        src = lib.cleanSourceWith {
          src = lib.cleanSource self;
          filter = name: _type: let
            base = baseNameOf name;
          in
            base != ".build" && base != "hyprgrid.so" && !(lib.hasPrefix "result" base);
        };

        # Lua comes from Hyprland's own inputs: the plugin must be built against
        # the Lua that runs it (constants like LUA_REGISTRYINDEX differ between
        # versions). Its interpreter also runs tests/layout.lua.
        nativeBuildInputs = [pkgs.pkg-config];
        buildInputs = [hyprlandPackage.dev] ++ hyprlandPackage.buildInputs;

        doCheck = true;
        checkTarget = "test";

        installPhase = ''
          runHook preInstall
          install -Dm755 hyprgrid.so $out/lib/libhyprgrid.so
          install -Dm644 hyprgrid.h $out/include/hyprgrid.h
          install -Dm644 hyprgrid.lua $out/share/hyprgrid/hyprgrid.lua
          runHook postInstall
        '';

        meta = {
          description = "2D workspace grid for Hyprland";
          homepage = "https://github.com/aetherall/hyprgrid";
          license = lib.licenses.bsd3;
          platforms = lib.platforms.linux;
        };
      };
  in {
    packages = eachSystem (system: rec {
      hyprgrid = mkHyprgrid nixpkgs.legacyPackages.${system} hyprland.packages.${system}.hyprland;
      default = hyprgrid;
    });

    # Builds against the `hyprland` in the package set it is applied to.
    overlays.default = final: _prev: {
      hyprgrid = mkHyprgrid final final.hyprland;
    };

    lib.mkHyprgrid = mkHyprgrid;

    devShells = eachSystem (system: let
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      default = pkgs.mkShell {
        name = "hyprgrid-dev";
        inputsFrom = [self.packages.${system}.hyprgrid];
        nativeBuildInputs = [pkgs.clang-tools];
      };
    });

    formatter = eachSystem (system: nixpkgs.legacyPackages.${system}.alejandra);
  };
}
