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
        "aarch64-darwin"
        "x86_64-darwin"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems f;
    in
    {
      devShells = forAllSystems (system: {
        default = nixpkgs.legacyPackages.${system}.mkShell {
          buildInputs = [
            xpg.packages.${system}.xpg
            nixpkgs.legacyPackages.${system}.curl
          ];
        };
      });
    };
}
