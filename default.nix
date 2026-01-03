{
  pkgs,
  lib,
  stdenv,
  cmake,
  which,
  ninja,
  clangCompiler,
  yaml-cpp,
  lit,
  filecheck,
  yq,
  jq,
  ruby,
  gtest,
  pandoc,
  qemu,
  llvmPackages,
  llvmLib,
  llvmSnippy,
  runTests ? false,
  ...
}:
let
  fs = lib.fileset;
  versionJson = builtins.fromJSON (builtins.readFile ./version.json);
in
stdenv.mkDerivation {
  pname = "llvm-bleach";
  version = versionJson.version;
  src = fs.toSource {
    root = ./.;
    fileset = fs.unions [
      ./CMakeLists.txt
      ./lib
      ./tools
      ./cmake
      ./include
      ./test
      ./docs
      ./version.json
    ];
  };
  nativeBuildInputs = [
    cmake
    ninja
    # for symtab operations
    clangCompiler
  ];
  buildInputs = [
    llvmLib
    yaml-cpp
    pandoc
  ];
  strictDeps = true;
  enableShared = false;
  nativeCheckInputs = [
    lit
    filecheck
    qemu
    llvmSnippy
    llvmPackages.bintools
    which
    clangCompiler
    yq
    jq
    ruby
  ]
  ++ (with pkgs; [
    pkgsCross.riscv64.buildPackages.clang
    pkgsCross.riscv64.buildPackages.llvmPackages_21.bintools
    pkgsCross.aarch64-multiplatform.buildPackages.llvmPackages_21.clang
  ]);
  preCheck = ''
    patchShebangs ..
  '';
  checkInputs = [ gtest ];
  doCheck = runTests;
}
