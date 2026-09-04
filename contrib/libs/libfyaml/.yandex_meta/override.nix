pkgs: attrs: with pkgs; rec {
  version = "0.9.4";

  src = fetchurl {
    url = "https://github.com/pantoniou/libfyaml/archive/v${version}.tar.gz";
    hash = "sha256-PLACEHOLDER";
  };

  patches = [
    ./pr0064-rename-list-symbols.patch
    ./pr0073-add-document-destroy-hook.patch
    ];
}
