{
  description = "llvm-bleach";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-parts,
      treefmt-nix,
      ...
    }@inputs:
    let
      systemToMusl = {
        "x86_64-linux" = "x86_64-unknown-linux-musl";
        "aarch64-linux" = "aarch64-unknown-linux-musl";
      };
    in
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [ treefmt-nix.flakeModule ];

      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      perSystem =
        { pkgs, system, ... }:
        rec {
          imports = [ ./nix/treefmt.nix ];

          legacyPackages = {
            bleachPkgsStatic = import nixpkgs {
              localSystem = {
                inherit system;
              };
              crossSystem = {
                inherit system;
                libc = "musl";
                config = systemToMusl.${system};
                isStatic = true;
              };
            };
            bleachPkgs = import nixpkgs { inherit system; };
          };
          packages =
            let
              normalPkgs = legacyPackages.bleachPkgs;
              staticPkgs = legacyPackages.bleachPkgsStatic;
              llvmPackages = normalPkgs.llvmPackages_21;
            in
            rec {
              llvm-bleach = normalPkgs.callPackage ./. {
                inherit self;
                inherit llvmPackages;
                inherit llvmSnippy;
                llvmLib = llvmPackages.llvm;
                clangCompiler = normalPkgs.clang;
              };
              llvm-bleach-static = staticPkgs.callPackage ./. {
                inherit llvmPackages;
                llvmLib = staticPkgs.llvmPackages_21.llvm;
                clangCompiler = normalPkgs.clang;
                inherit llvmSnippy;
                # FIXME: qemu refuses to build
                runTests = false;
                inherit (normalPkgs)
                  lit
                  filecheck
                  yq
                  jq
                  rubygtest
                  pandoc
                  ;
              };
              default = llvm-bleach;
              llvmSnippy = pkgs.callPackage ./snippy.nix {
                stdenv = llvmPackages.stdenv;
              };
            };
          checks = {
            inherit (packages) llvm-bleach;
          };
          devShells.default = pkgs.mkShell {
            nativeBuildInputs =
              packages.llvm-bleach.nativeBuildInputs
              ++ (with pkgs; [
                doxygen
                clang-tools
                lit
                filecheck
                act
                lldb
                gdb
                valgrind
                just
              ]);
            buildInputs = packages.llvm-bleach.buildInputs;
          };
          devShells.withBleach = pkgs.mkShell { nativeBuildInputs = [ packages.llvm-bleach-static ]; };
        };
    };
}
