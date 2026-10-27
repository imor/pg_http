{ writeShellScriptBin }:

writeShellScriptBin "http-loadtest" (builtins.readFile ./loadtest.sh)
