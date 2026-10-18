<powershell>
# The image build's user data (windows.pkr.hcl): the build's Administrator
# password, and WinRM over HTTPS for Packer (RunsOn's image leaves it disabled).
net user Administrator "${password}"
Set-Service -Name WinRM -StartupType Automatic
Start-Service -Name WinRM
Enable-PSRemoting -SkipNetworkProfileCheck -Force
winrm set winrm/config/service/auth '@{Basic="true"}'
Get-ChildItem WSMan:\Localhost\Listener | Remove-Item -Recurse
$cert = New-SelfSignedCertificate -CertStoreLocation Cert:\LocalMachine\My -DnsName "opennova-ci-image"
New-Item -Path WSMan:\LocalHost\Listener -Transport HTTPS -Address * -CertificateThumbPrint $cert.Thumbprint -Force
New-NetFirewallRule -DisplayName "OpenNova image build: WinRM over HTTPS" -Name "OpenNova-WinRM-HTTPS-In" -Profile Any -LocalPort 5986 -Protocol TCP
</powershell>
<persist>false</persist>
