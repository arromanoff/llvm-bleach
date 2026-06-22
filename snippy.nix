{
  pkgs,
  stdenv,
  lib,
  cmake,
  ninja,
  python3,
  gtest,
  ...
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "llvm-snippy";
  version = "3.0.0";

  src = pkgs.fetchFromGitHub {
    owner = "LLVM-Snippy";
    repo = "llvm-snippy";
    rev = "794b293be118fee81c00aaa2d48a1c029d75c66c";
    hash = "sha256-N4SwHy+yu8TKqaKXue39HWOHqVyY7Sx2s8tw8y4bDRc=";
  };
  patches = [ ./nix/llvm-ie-linker.patch ];

  sourceRoot = "${finalAttrs.src.name}/llvm";
  strictDeps = true;
  nativeBuildInputs = [
    cmake
    ninja
    python3
  ];
  buildInputs = [ ];

  cmakeFlags = [
    (lib.cmakeFeature "LLVM_SNIPPY_VERSION" finalAttrs.version)
    (lib.cmakeBool "LLVM_ENABLE_RTTI" false)
    (lib.cmakeBool "LLVM_BUILD_SNIPPY" true)
    (lib.cmakeBool "LLVM_ENABLE_ASSERTIONS" true)
    (lib.cmakeBool "LLVM_BUILD_TESTS" finalAttrs.finalPackage.doCheck)
    (lib.cmakeBool "LLVM_INCLUDE_UTILS" finalAttrs.finalPackage.doCheck)
    (lib.cmakeBool "LLVM_INCLUDE_TESTS" finalAttrs.finalPackage.doCheck)
    (lib.cmakeBool "LLVM_INCLUDE_BENCHMARKS" false)
    (lib.cmakeFeature "LLVM_TARGETS_TO_BUILD" "RISCV;AArch64")
    (lib.cmakeFeature "LLVM_ENABLE_PROJECTS" "lld")
  ];

  installTargets = [
    "install-llvm-snippy"
    "install-llvm-ie"
  ];

  ninjaFlags = [
    "llvm-snippy"
    "llvm-ie"
  ];

  doCheck = true;
  nativeCheckInputs = [ ];
  checkInputs = [ gtest ];
  checkTarget = "check-llvm-tools-llvm-snippy";
})
