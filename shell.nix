{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = [
    pkgs.gcc
    pkgs.gnumake
    pkgs.pkg-config
    pkgs.catch2          # v2.x, header path: catch2/catch.hpp
    pkgs.spdlog
    pkgs.fmt
    pkgs.curl
    pkgs.SDL2            # lvgl SDL driver (USE_SIMULATOR sim + host tests)
    pkgs.libpng         # lvgl png decoder + find_package(PNG)
  ];
}
