{
  description = "tupu: a Datalog engine in C";

  inputs.nixpkgs.url = "https://nixos.org/channels/nixos-unstable/nixexprs.tar.xz";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "tupu";
          version = "0.0.0";
          src = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.unions [ ./Justfile ./src ];
          };
          nativeBuildInputs = [ pkgs.just pkgs.gcc ];
          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            just build
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tupu $out/bin/tupu
            runHook postInstall
          '';
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ gcc clang just mcpp hyperfine perf valgrind ];
        };
      });

      checks = forAll (pkgs: {
        default = pkgs.runCommand "tupu-test"
          {
            src = ./.;
            nativeBuildInputs = with pkgs; [ gcc clang just mcpp coreutils gawk gnugrep gnused findutils diffutils ];
          }
          ''
            cp -r $src work
            chmod -R u+w work
            cd work
            just test
            touch $out
          '';
      });
    };
}
