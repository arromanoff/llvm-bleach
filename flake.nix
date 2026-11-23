{
  description = "llvm-bleach";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    snippy = {
      url = "github:LLVM-Snippy/llvm-snippy";
      flake = false;
    };
    llvm-project = {
      url = "github:arromanoff/llvm-project/tms320";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      flake-parts,
      treefmt-nix,
      snippy,
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
              # Keep LLVM and MLIR on the same fork revision, in both variants.
              forkLLVM =
                pkgs:
                (pkgs.llvmPackages_23.override {
                  version = "24.0.0";
                  # Nixpkgs' subproject source derivations copy this attribute.
                  monorepoSrc = inputs.llvm-project // {
                    passthru = { };
                  };
                }).overrideScope
                  (
                    _: prev: {
                      # Build-time generators must match the fork too. Use native
                      # tools even when the libraries target static musl.
                      libllvm =
                        (prev.libllvm.override {
                          buildLlvmPackages = llvmPackages;
                        }).overrideAttrs
                          (old: {
                            patches = map (
                              patch:
                              if pkgs.lib.hasSuffix "llvm-lit-cfg-add-libs-to-dylib-path.patch" (toString patch) then
                                # LLVM 24 corrected the comment and added z/OS setup
                                # after this hunk, which previously matched EOF.
                                builtins.toFile "llvm-lit-cfg-add-libs-to-dylib-path.patch" (
                                  builtins.replaceStrings
                                    [
                                      "propogate."
                                      "@@ -55,3 +56,26 @@"
                                      "diff --git a/test/lit.cfg.py"
                                    ]
                                    [
                                      "propagate."
                                      "@@ -55,5 +56,28 @@"
                                      " \n # The unit tests on z/OS need auto-conversion turned on.\ndiff --git a/test/lit.cfg.py"
                                    ]
                                    (builtins.readFile patch)
                                )
                              else
                                patch
                            ) old.patches;
                            postPatch = (old.postPatch or "") + ''
                              # The PPC test uses ld.bfd, not gold; don't take its
                              # emulation support from the gold probe.
                              substituteInPlace test/lit.cfg.py \
                                --replace-fail 'if "elf32ppc" in emulations:' \
                                  'if name == "GNU ld" and "elf32ppc" in emulations:'
                            '';
                          });
                      mlir =
                        (prev.mlir.override {
                          buildLlvmPackages = llvmPackages;
                        }).overrideAttrs
                          (old: {
                            # Already present in the fork's LLVM 24 sources.
                            patches = builtins.filter (
                              patch: !(pkgs.lib.hasSuffix "mlir-tablegen-imported-target.patch" (toString patch))
                            ) old.patches;
                          });
                      lld = prev.lld.override {
                        buildLlvmPackages = llvmPackages;
                      };
                    }
                  );
              llvmPackages = forkLLVM normalPkgs;
              staticLLVM = forkLLVM staticPkgs;
            in
            rec {
              llvm-bleach = normalPkgs.callPackage ./. {
                stdenv = normalPkgs.gcc16Stdenv;
                inherit self;
                inherit llvmPackages;
                inherit llvmSnippy;
                llvmLib = llvmPackages.llvm;
                mlir = llvmPackages.mlir;
                clangCompiler = normalPkgs.llvmPackages_23.clang;
              };
              llvm-bleach-static = staticPkgs.callPackage ./. {
                llvmPackages = staticLLVM;
                stdenv = staticPkgs.gcc16Stdenv;
                mlir = staticLLVM.mlir;
                llvmLib = staticLLVM.llvm;
                clangCompiler = normalPkgs.llvmPackages_23.clang;
                inherit llvmSnippy;
                inherit (normalPkgs)
                  lit
                  filecheck
                  yq
                  jq
                  ruby
                  rubygtest
                  pandoc
                  gtest
                  qemu
                  ;
              };
              default = llvm-bleach;
              llvmSnippy = import ./nix/snippy.nix {
                pkgs = normalPkgs;
                inherit snippy;
              };
            };
          checks = {
            inherit (packages) llvm-bleach;
          };
          devShells.default = (pkgs.mkShell.override { stdenv = packages.llvm-bleach.stdenv; }) {
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
