# Comparators of step 4 (S94): Souffle 2.5 with 64-bit numbers at the release flags of its CMake build (no -O override),
# and FlowLog as its flake builds it. Build each as a background job (tools/bench-build.sh); sources stay in the nix store.
{ pkgs ? import <nixpkgs> { }, flowlogSrc ? /home/vic/hk/FlowLog }:
{
  souffle64 = pkgs.souffle.overrideAttrs (old: {
    pname = "souffle64";
    cmakeFlags = old.cmakeFlags ++ [ "-DSOUFFLE_DOMAIN_64BIT=ON" ];
  });
  flowlog = pkgs.rustPlatform.buildRustPackage {
    pname = "flowlog";
    version = "git";
    src = flowlogSrc;
    cargoLock.lockFile = "${flowlogSrc}/Cargo.lock";
    cargoBuildFlags = [ "-p" "flowlog-compiler" ];
    doCheck = false;
  };
}
