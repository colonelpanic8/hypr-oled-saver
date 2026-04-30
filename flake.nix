{
  description = "hypr-oled-saver, an OLED-friendly Hyprland layer-shell screensaver";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default-linux";
  };

  outputs = {
    self,
    nixpkgs,
    systems,
    ...
  }: let
    inherit (nixpkgs) lib;
    eachSystem = lib.genAttrs (import systems);
    pkgsFor = eachSystem (system:
      import nixpkgs {
        localSystem.system = system;
      });
    sourceFiles = [
      "src/main.cpp"
    ];
    sourceFileArgs = lib.concatMapStringsSep " " lib.escapeShellArg sourceFiles;
  in {
    packages = eachSystem (system: let
      pkgs = pkgsFor.${system};
    in {
      default = pkgs.stdenv.mkDerivation {
        pname = "hypr-oled-saver";
        version = "0.1.0";
        src = builtins.path {
          path = ./.;
          name = "hypr-oled-saver-source";
        };

        nativeBuildInputs = [
          pkgs.cmake
          pkgs.pkg-config
          pkgs.wrapGAppsHook3
        ];

        buildInputs = [
          pkgs.gtk3
          pkgs.gtk-layer-shell
          pkgs.nlohmann_json
        ];

        meta = {
          description = "An OLED-friendly Hyprland layer-shell screensaver";
          homepage = "https://github.com/colonelpanic8/hypr-oled-saver";
          license = lib.licenses.bsd3;
          platforms = lib.platforms.linux;
          mainProgram = "hypr-oled-saver";
        };
      };

      hypr-oled-saver = self.packages.${system}.default;
    });

    checks = eachSystem (system: let
      pkgs = pkgsFor.${system};
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
    in {
      default = pkgs.mkShell {
        name = "hypr-oled-saver";
        nativeBuildInputs = [
          pkgs.cmake
          pkgs.clang-tools
          pkgs.pkg-config
          pkgs.wrapGAppsHook3
        ];
        buildInputs = [
          pkgs.gtk3
          pkgs.gtk-layer-shell
          pkgs.nlohmann_json
        ];
      };
    });
  };
}
