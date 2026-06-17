namespace OpenNova.Launcher.Hosts;

/// <summary>A single hostname to IPv4 redirection managed by the launcher.</summary>
public readonly record struct HostsRedirect(string Hostname, string IPv4);
