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
  ];
}
