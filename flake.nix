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
            exec "$(git rev-parse --show-toplevel)/scripts/.venv/bin/qqu" "$@"
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
      };
    };
}
