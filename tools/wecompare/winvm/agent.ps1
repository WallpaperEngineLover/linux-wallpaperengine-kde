# runs in the VM's logged on session (scheduled at logon by install_agent.ps1): every *.ps1 that winvm.sh run drops
# into \\10.0.2.4\qemu\cmd is executed once, its output goes to \\10.0.2.4\qemu\out\<name>.txt and <name>.done marks
# the end. In the user session, not a service, so programs it starts get the desktop
$share = '\\10.0.2.4\qemu'
New-Item -ItemType Directory -Force "$share\cmd", "$share\out" | Out-Null
while ($true) {
    Get-ChildItem "$share\cmd" -Filter *.ps1 -ErrorAction SilentlyContinue | ForEach-Object {
        $name = $_.BaseName
        $script = "$env:TEMP\agent_$name.ps1"
        Move-Item $_.FullName $script -Force
        try {
            & powershell -NoProfile -ExecutionPolicy Bypass -File $script *> "$share\out\$name.txt"
        } catch {
            $_ | Out-File -Append "$share\out\$name.txt"
        }
        New-Item -ItemType File -Force "$share\out\$name.done" | Out-Null
    }
    Start-Sleep -Milliseconds 500
}
