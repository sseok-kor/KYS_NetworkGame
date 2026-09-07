using System.Text;

namespace DataEditor.Loader;

/// <summary>다파일 커밋의 flip 단계에서 일부 파일만 교체된 뒤 실패했음을 알린다(부분 커밋·되돌릴 수 없음).
/// 호출자는 이를 "원본 보존됨"과 구분해 사용자에게 정확히 알려야 한다(리뷰 F14).</summary>
public sealed class PartialCommitException : Exception
{
    public int Committed { get; }
    public int Total { get; }
    public PartialCommitException(int committed, int total, Exception inner)
        : base($"다파일 커밋 중 {committed}/{total} 파일 교체 후 실패 - 일부 파일만 갱신됨", inner)
    { Committed = committed; Total = total; }
}

/// <summary>
/// UTF-8 텍스트 파일 읽기/쓰기 + BOM·개행 감지·보존(round-trip 충실도·리뷰 #18).
/// 리포 데이터 파일은 대부분 no-BOM·LF(walkable만 BOM 있을 수 있음)이나,
/// 감지한 값을 그대로 되쓰므로 어느 경우든 원본을 보존한다.
/// </summary>
public static class TextFileIo
{
    private static readonly byte[] Utf8Bom = { 0xEF, 0xBB, 0xBF };

    /// <summary>파일을 읽어 (BOM 제거된 텍스트, 감지한 BOM(or null), 감지한 개행)을 반환.</summary>
    public static (string text, byte[]? bom, string newline) ReadText(string path)
    {
        var bytes = File.ReadAllBytes(path);
        byte[]? bom = null;
        int start = 0;
        if (bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            bom = Utf8Bom;
            start = 3;
        }
        var text = Encoding.UTF8.GetString(bytes, start, bytes.Length - start);
        // CRLF가 하나라도 있으면 CRLF 파일로 간주(리포 실측은 전부 LF).
        var newline = text.Contains("\r\n") ? "\r\n" : "\n";
        return (text, bom, newline);
    }

    /// <summary>바이트를 temp 파일에 쓰고 원자 교체(rename). File.Replace는 mtime 보존 footgun이라
    /// File.Move(overwrite=true)로 대체 — temp의 write time이 찍혀 walkable .bin 캐시 무효화가 동작(리뷰 #11).</summary>
    public static void WriteBytesAtomic(string path, byte[] payload)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(path))!;
        var tmp = Path.Combine(dir, Path.GetFileName(path) + ".tmp" + Guid.NewGuid().ToString("N"));
        File.WriteAllBytes(tmp, payload);
        File.Move(tmp, path, overwrite: true); // rename: temp의 최신 mtime을 대상에 부여
    }

    /// <summary>
    /// 여러 파일을 stage-all-then-flip 으로 커밋(다파일 준-원자성·리뷰 #2).
    /// 1) 모든 대상을 temp 로 먼저 쓴다 — 하나라도 실패하면 이미 쓴 temp 를 지우고 예외 재throw(정본 파일 무변경 = 부분 상태 0).
    /// 2) 전부 성공하면 rename 배치로 뒤집는다(부분-쓰기 창 = rename 루프뿐). 파일당 rename 은 near-atomic.
    /// 3) deletePaths(예: walkable .bin 캐시)를 지운다 — 없으면 무시.
    /// 진정한 다파일 원자성(저널/2PC)은 후속. 실무 통용 staging 패턴으로 실패-전-flip 부분상태를 제거.
    /// </summary>
    public static void WriteAllAtomic(
        IReadOnlyList<(string path, byte[] payload)> files,
        IReadOnlyList<string>? deletePaths = null)
    {
        var staged = new List<(string tmp, string final)>(files.Count);
        try
        {
            foreach (var (path, payload) in files)
            {
                var full = Path.GetFullPath(path);
                var dir = Path.GetDirectoryName(full)!;
                var tmp = Path.Combine(dir, Path.GetFileName(full) + ".editortmp" + Guid.NewGuid().ToString("N"));
                File.WriteAllBytes(tmp, payload);
                staged.Add((tmp, full));
            }
        }
        catch
        {
            foreach (var (tmp, _) in staged) TryDelete(tmp);
            throw; // 아직 어떤 정본도 건드리지 않음 - 호출자는 원본 그대로를 본다
        }

        // flip: 모든 temp 가 성공적으로 쓰였으니 이제 정본으로 교체(작은 창).
        //   rename 이 중간에 실패(게임이 파일 점유·읽기전용·AV 스캔 등)할 수 있으므로 감싼다 —
        //   이미 교체된 파일은 되돌릴 수 없으나(진짜 다파일 원자성=저널 필요), 남은 temp 는 정리하고
        //   부분 커밋을 호출자에 정확히 알려 "원본 보존됨" 허위 보고를 막는다(리뷰 F14).
        int flipped = 0;
        try
        {
            for (; flipped < staged.Count; flipped++)
                File.Move(staged[flipped].tmp, staged[flipped].final, overwrite: true); // temp 의 최신 mtime 부여(.bin 무효화)
        }
        catch (Exception ex)
        {
            for (int i = flipped; i < staged.Count; i++) TryDelete(staged[i].tmp); // 미교체 temp 정리
            throw new PartialCommitException(flipped, staged.Count, ex);
        }

        if (deletePaths != null)
            foreach (var p in deletePaths) TryDelete(p);
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); }
        catch { /* 캐시/temp 삭제 실패는 무해 - 무시 */ }
    }

    /// <summary>walkable CSV 파일명 → 짝 .bin 캐시 파일명(게임 규약: 확장자만 .bin 으로 교체).</summary>
    public static string WalkableBinName(string csvFileName) => Path.ChangeExtension(csvFileName, ".bin");

    public static byte[] ComposeBytes(string text, byte[]? bom)
    {
        var payload = Encoding.UTF8.GetBytes(text);
        if (bom == null) return payload;
        var outb = new byte[bom.Length + payload.Length];
        Array.Copy(bom, 0, outb, 0, bom.Length);
        Array.Copy(payload, 0, outb, bom.Length, payload.Length);
        return outb;
    }
}
