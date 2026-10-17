{
  description = "tonarchy - Minimal Arch Linux installer";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = fn: nixpkgs.lib.genAttrs systems (system: fn nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        build_iso = pkgs.stdenv.mkDerivation {
          pname = "build_iso";
          version = "0.1.0";
          src = ./.;
          buildInputs = [ pkgs.musl ];
          buildPhase = ''
            ${pkgs.musl.dev}/bin/musl-gcc -std=c23 -Wall -Wextra -O2 -static src/build_iso.c -o build_iso
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp build_iso $out/bin/
          '';
        };

        default = self.packages.${pkgs.system}.build_iso;
      });

      apps = forAllSystems (pkgs: {
        build_iso = {
          type = "app";
          program = "${self.packages.${pkgs.system}.build_iso}/bin/build_iso";
        };

        default = self.apps.${pkgs.system}.build_iso;
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          buildInputs = [
            pkgs.gcc
            pkgs.glibc.static
            pkgs.gnumake
            pkgs.bear
            pkgs.qemu_kvm
            pkgs.OVMF
            pkgs.podman
            pkgs.distrobox
          ];
          shellHook = ''
            export PS1="(tonarchy-dev) $PS1"
            echo "tonarchy development environment"
            echo "Run 'make' to build"
            echo "Run 'make build' to build ISO natively (Arch)"
            echo "Run 'nix run .#build_iso -- --container podman' to build ISO (NixOS)"
            echo "Run './vm-test [iso-path]' to test an ISO in qemu"
          '';
        };
      });
    };
}
