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
  version = "2.1.0";

  src = pkgs.fetchFromGitHub {
    owner = "LLVM-Snippy";
    repo = "llvm-snippy";
    rev = "169f6df67fd0b2acc661b4e0cb95e090ecc1bdad";
    hash = "sha256-chFUH/aVGAsLc7s7HgSP5iVFcNsEK9J9eWdxpz6C3GY=";
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
    (lib.cmakeFeature "LLVM_TARGETS_TO_BUILD" "RISCV")
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
