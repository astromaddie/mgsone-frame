$ErrorActionPreference = 'Stop'

$Distro = 'Ubuntu'
$Workspace = '/home/astro/MGSOneFrameDev/source'

& wsl.exe -d $Distro --exec test -d $Workspace
if ($LASTEXITCODE -ne 0) {
    throw "The WSL project checkout is missing at $Workspace."
}

& wsl.exe -d $Distro --cd $Workspace --exec bash -l
exit $LASTEXITCODE
