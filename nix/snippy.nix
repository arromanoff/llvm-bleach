{
  pkgs,
  snippy,
}:

let
  sources = import "${snippy}/llvm/tools/llvm-snippy/nix/pins";

  mkScopeFor =
    pkgs:
    let
      newScope = extra: pkgs.newScope extra;
    in
    pkgs.lib.makeScopeWithSplicing'
      {
        inherit newScope;
        inherit (pkgs) splicePackages;
      }
      {
        otherSplices = pkgs.lib.renameCrossIndexTo "self" (
          pkgs.lib.mapCrossIndex
            (pkgs': mkScopeFor pkgs')
            (pkgs.lib.renameCrossIndexFrom "pkgs" pkgs)
        );

        f = self:
          import "${snippy}/llvm/tools/llvm-snippy/nix/components.nix" {
            inherit sources self;
            lib = pkgs.lib;
          };
      };

in
(mkScopeFor pkgs).llvm-snippy
