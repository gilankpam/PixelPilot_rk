{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = [
    pkgs.cmake
    pkgs.pkg-config
    pkgs.SDL2
    pkgs.libpng
    pkgs.cairo
    pkgs.spdlog
    pkgs.fmt
  ];
}
