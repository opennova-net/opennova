namespace OpenNova.Launcher.Hosts;

/// <summary>One hostname to IPv4 line in the launcher's managed block.</summary>
public readonly record struct HostsRedirect(string Hostname, string IPv4);
