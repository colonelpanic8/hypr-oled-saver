{
  description = "hypr-oled-saver, an OLED-friendly Hyprland screensaver plugin";

  inputs = {
    hyprland.url = "git+https://github.com/hyprwm/Hyprland?submodules=1";
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
    pkgsFor = eachSystem (system:
      import nixpkgs {
        localSystem.system = system;
        overlays = [hyprland.overlays.hyprland-packages];
      });
    sourceFiles = [
      "src/main.cpp"
      "src/standalone.cpp"
    ];
    sourceFileArgs = lib.concatMapStringsSep " " lib.escapeShellArg sourceFiles;
  in {
    packages = eachSystem (system: let
      pkgs = pkgsFor.${system};
      hyprlandPkg = hyprland.packages.${system}.hyprland;
    in {
      default = pkgs.hyprlandPlugins.mkHyprlandPlugin {
        pluginName = "hypr-oled-saver";
        version = "0.1.0";
        src = builtins.path {
          path = ./.;
          name = "hypr-oled-saver-source";
        };

        inherit (hyprlandPkg) nativeBuildInputs;
        buildInputs = [];

        meta = {
          description = "An OLED-friendly Hyprland screensaver plugin";
          homepage = "https://github.com/colonelpanic8/hypr-oled-saver";
          license = lib.licenses.bsd3;
          platforms = lib.platforms.linux;
        };
      };

      hypr-oled-saver = self.packages.${system}.default;
    });

    checks = eachSystem (system: let
      pkgs = pkgsFor.${system};
      hyprlandPkg = hyprland.packages.${system}.hyprland;
      src = builtins.path {
        path = ./.;
        name = "hypr-oled-saver-source";
      };
    in {
      hypr-oled-saver = self.packages.${system}.default;

      clang-format = pkgs.runCommand "hypr-oled-saver-clang-format-check" {
        inherit src;
        nativeBuildInputs = [pkgs.clang-tools];
      } ''
        cd "$src"
        clang-format --dry-run --Werror ${sourceFileArgs}
        touch "$out"
      '';
    });

    devShells = eachSystem (system: let
      pkgs = pkgsFor.${system};
      hyprlandPkg = hyprland.packages.${system}.hyprland;
    in {
      default = pkgs.mkShell.override {stdenv = pkgs.gcc14Stdenv;} {
        name = "hypr-oled-saver";
        nativeBuildInputs = [
          pkgs.clang-tools
        ];
        buildInputs = [hyprlandPkg];
        inputsFrom = [
          hyprlandPkg
          self.packages.${system}.default
        ];
      };
    });
  };
}
