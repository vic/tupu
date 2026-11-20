{ pkgs ? import <nixpkgs> { } }:
pkgs.mkShell {
  packages = with pkgs; [ gcc clang gnumake mcpp valgrind linuxPackages.perf time ];
}
