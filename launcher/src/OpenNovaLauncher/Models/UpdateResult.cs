namespace OpenNova.Launcher.Models;

public readonly struct UpdateResult
{
    public UpdateResult(bool isSuccess, string message)
    {
        IsSuccess = isSuccess;
        Message = message;
    }

    public bool IsSuccess { get; }
    public string Message { get; }

    public static UpdateResult FromSuccess(string message) => new(true, message);
    public static UpdateResult FromFailure(string message) => new(false, message);
}
