{
  description = "low latency dev environment";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      llvm = pkgs.llvmPackages_22;
      # gcc16's cc-wrapper setup-hook exports CXX=g++ unconditionally, so link
      # only its /bin to keep g++ available for A/B without hijacking CXX
      gpp = pkgs.buildEnv {
        name = "gpp-only";
        paths = [ pkgs.gcc16 ];
        pathsToLink = [ "/bin" ];
      };
    in
    {
      devShells.${system}.default = llvm.libcxxStdenv.mkDerivation {
        name = "qqu-dev-shell";
        nativeBuildInputs = with pkgs; [
          (writeShellScriptBin "qqu" ''
            root="$(git rev-parse --show-toplevel)"
            if [ ! -x "$root/scripts/.venv/bin/qqu" ]; then
              echo "qqu: scripts/.venv is missing; re-enter 'nix develop'" >&2
              exit 1
            fi
            exec "$root/scripts/.venv/bin/qqu" "$@"
          '')
          cmake
          gdb
          gpp
          git
          llvm.clang-tools
          ninja
          perf
          python3
          util-linux # taskset (CPU pinning in benchmarks)
          uv
        ];
        # uv-installed wheels (matplotlib/numpy) link system libstdc++
        # NIX_ENFORCE_NO_NATIVE strips -march=native, needed for accurate benches
        env = {
          LD_LIBRARY_PATH = "${pkgs.stdenv.cc.cc.lib}/lib";
          NIX_ENFORCE_NO_NATIVE = "";
        };
        # qqu is a wrapper around the Python harness; create its venv once
        shellHook = ''
          root="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
          if [ ! -x "$root/scripts/.venv/bin/qqu" ]; then
            uv venv "$root/scripts/.venv"
            uv pip install --python "$root/scripts/.venv/bin/python" -e "$root/scripts"
          fi
        '';
      };
    };
}
