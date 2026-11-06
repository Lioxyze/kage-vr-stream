# Downloads the LibOVR headers/shim used for Rift CV1 full body tracking (from the
# MIT-licensed OculusTouchLink project, which ships them) into tools/otsl.
$root = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path "$root\tools\otsl")) { git clone --depth 1 https://github.com/mm0zct/Oculus_Touch_Steam_Link "$root\tools\otsl" }
