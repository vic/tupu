{
  description = "tupu: a Datalog engine in C";

  inputs.nixpkgs.url = "https://nixos.org/channels/nixos-unstable/nixexprs.tar.xz";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: rec {
        souffle-compat = pkgs.writeShellApplication {
          name = "souffle";
          runtimeInputs = [ pkgs.mcpp default pkgs.coreutils pkgs.gawk pkgs.gnugrep ];
          text = builtins.readFile ./souffle-compat.sh;
        };
        default = pkgs.stdenv.mkDerivation {
          pname = "tupu";
          version = "0.0.0";
          src = pkgs.lib.fileset.toSource {
            root = ./.;
            fileset = pkgs.lib.fileset.unions [ ./Justfile ./src ./include ];
          };
          nativeBuildInputs = [ pkgs.just pkgs.gcc ];
          dontConfigure = true;
          buildPhase = ''
            runHook preBuild
            just build lib
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            install -Dm755 build/tupu $out/bin/tupu
            install -Dm644 build/libtupu.a $out/lib/libtupu.a
            install -Dm644 include/tupu.h $out/include/tupu.h
            runHook postInstall
          '';
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ gcc clang just mcpp hyperfine perf valgrind cargo rustc ];
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
            mkdir -p result/bin
            cp ${self.packages.${pkgs.system}.default}/bin/tupu result/bin/tupu
            SKIP_O7=1 just test
            touch $out
          '';
      });
    };
}
