# packaging/nix/default.nix — agentty as a Nix package.
#
# @VERSION@ / @LINUX_X86_64_SHA256@ / @LINUX_AARCH64_SHA256@ are rewritten from
# CMakeLists.txt (project VERSION) + the release SHA256SUMS by
# scripts/release.sh. This is a TEMPLATE — do not hardcode the version.
#
# Install (flake-free, ad-hoc):
#   nix-env -iA agentty -f https://github.com/1ay1/agentty/archive/master.tar.gz
# Or add to your configuration.nix / home-manager as an overlay.
#
# agentty ships as a fully-static musl binary, so we fetch the published
# release artifact rather than compiling C++26 from source under nixpkgs.
{ lib
, stdenvNoCC
, fetchurl
, autoPatchelfHook ? null   # not needed for the fully-static build
}:

let
  version = "@VERSION@";

  # arch → (release asset name, sha256) from the published SHA256SUMS.
  sources = {
    "x86_64-linux" = {
      name = "agentty-linux-x86_64";
      sha256 = "@LINUX_X86_64_SHA256@";
    };
    "aarch64-linux" = {
      name = "agentty-linux-aarch64";
      sha256 = "@LINUX_AARCH64_SHA256@";
    };
  };

  src = sources.${stdenvNoCC.hostPlatform.system}
    or (throw "agentty: unsupported platform ${stdenvNoCC.hostPlatform.system}");
in
stdenvNoCC.mkDerivation {
  pname = "agentty";
  inherit version;

  src = fetchurl {
    url = "https://github.com/1ay1/agentty/releases/download/v${version}/${src.name}";
    inherit (src) sha256;
  };

  # The binary is fully static (musl), so no patchelf / dynamic loader fixups.
  dontUnpack = true;
  dontPatchELF = true;
  dontStrip = true;

  installPhase = ''
    runHook preInstall
    install -Dm755 "$src" "$out/bin/agentty"
    runHook postInstall
  '';

  meta = with lib; {
    description = "Blazing-fast Claude in your terminal — single static binary, sandboxed, airgap-capable";
    homepage = "https://github.com/1ay1/agentty";
    license = licenses.mit;
    mainProgram = "agentty";
    platforms = [ "x86_64-linux" "aarch64-linux" ];
    maintainers = [ ];
  };
}
