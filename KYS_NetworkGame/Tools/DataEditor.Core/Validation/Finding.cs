using DataEditor.Model;

namespace DataEditor.Validation;

/// <summary>검증 결과 한 건. severity는 게임 부팅 fail-fast 기준과 1:1(Error=부팅 거부/Warning=행 무시).</summary>
public sealed class Finding
{
    public FindingSeverity Severity { get; init; }
    public string Category { get; init; } = "";   // maps/walkable/portals/items/monsters/spawns
    public object? Entity { get; init; }           // 위반 엔티티(캔버스 포커스용)
    public string Message { get; init; } = "";

    public override string ToString() => $"[{Severity}] {Category}: {Message}";
}
