{ pkgs ? import <nixpkgs> { } }:
pkgs.mkShell {
  packages = with pkgs; [ gcc clang just mcpp valgrind perf time ];
}
