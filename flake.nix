{
  description = "Development shell for pg_http";

  inputs = {
    xpg.url = "github:supabase/xpg/v2.5.0";
    nixpkgs.follows = "xpg/nixpkgs";
  };

  outputs =
    { nixpkgs, xpg, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems f;
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          format = pkgs.writeShellScriptBin "format" ''
            ${pkgs.clang-tools}/bin/clang-format -i $(find src -name '*.c' -o -name '*.h')
            ${pkgs.findutils}/bin/find . -name '*.nix' -exec ${pkgs.nixfmt}/bin/nixfmt {} \+
          '';
          formatCheck = pkgs.writeShellScriptBin "format-check" ''
            ${pkgs.clang-tools}/bin/clang-format -i $(find src -name '*.c' -o -name '*.h')
            ${pkgs.git}/bin/git diff-index --exit-code HEAD -- '*.c' '*.h'
            ${pkgs.findutils}/bin/find . -name '*.nix' -exec ${pkgs.nixfmt}/bin/nixfmt --check {} \+
          '';
        in
        {
          default = pkgs.mkShell {
            buildInputs = [
              xpg.packages.${system}.xpg
              pkgs.curl
              format
              formatCheck
            ];
          };
        }
      );
    };
}
