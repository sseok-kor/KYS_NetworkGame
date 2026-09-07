using System.Text;
using DataEditor.Loader;
using Xunit;

namespace DataEditor.Tests;

/// <summary>
/// 다파일 원자 커밋(WriteAllAtomic) 게이트 — 스크래치 디렉터리에서만 검증(리포 Data/ 무접촉).
/// (1) 정상: 모든 정본 교체 + 짝 .bin 삭제 + temp 잔여 0.
/// (2) 실패-전-flip: 한 대상의 temp 쓰기가 실패하면 정본 무변경(부분 상태 0) + temp 정리.
/// </summary>
public class WriterTests
{
    private static string ScratchDir()
    {
        var d = Path.Combine(Path.GetTempPath(), "DataEditorTest_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(d);
        return d;
    }

    [Fact]
    public void WriteAllAtomic_commits_all_files_and_deletes_companion_bin()
    {
        var d = ScratchDir();
        try
        {
            var a = Path.Combine(d, "a.csv");
            var b = Path.Combine(d, "walkable_map0.csv");
            var bin = Path.Combine(d, "walkable_map0.bin");
            File.WriteAllText(a, "old-a");
            File.WriteAllText(b, "old-b");
            File.WriteAllBytes(bin, new byte[] { 1, 2, 3 }); // 낡은 캐시

            TextFileIo.WriteAllAtomic(
                new (string, byte[])[]
                {
                    (a, Encoding.UTF8.GetBytes("new-a")),
                    (b, Encoding.UTF8.GetBytes("new-b")),
                },
                new[] { bin });

            Assert.Equal("new-a", File.ReadAllText(a));
            Assert.Equal("new-b", File.ReadAllText(b));
            Assert.False(File.Exists(bin), "짝 .bin 캐시가 삭제되어야 함");
            Assert.Empty(Directory.GetFiles(d, "*editortmp*"));
        }
        finally { Directory.Delete(d, true); }
    }

    // F14: flip 단계에서 File.Move 가 실패하면 남은 temp 를 정리하고 PartialCommitException 으로 부분 커밋을 알린다.
    [Fact]
    public void WriteAllAtomic_flip_failure_reports_partial_commit_and_cleans_temps()
    {
        var d = ScratchDir();
        try
        {
            var a = Path.Combine(d, "a.csv");
            File.WriteAllText(a, "old-a");
            var bDir = Path.Combine(d, "b.csv");
            Directory.CreateDirectory(bDir); // b.csv 를 디렉터리로 만들어 File.Move 교체 실패 유발

            var ex = Assert.Throws<PartialCommitException>(() =>
                TextFileIo.WriteAllAtomic(new (string, byte[])[]
                {
                    (a, Encoding.UTF8.GetBytes("new-a")),
                    (bDir, Encoding.UTF8.GetBytes("new-b")),
                }));

            Assert.Equal(1, ex.Committed);                  // a 는 교체됨, b(디렉터리)에서 실패
            Assert.Equal(2, ex.Total);
            Assert.Equal("new-a", File.ReadAllText(a));      // 이미 flip 된 a 는 커밋됨(되돌릴 수 없음)
            Assert.Empty(Directory.GetFiles(d, "*editortmp*")); // 미교체 temp 는 정리됨
        }
        finally { Directory.Delete(d, true); }
    }

    [Fact]
    public void WriteAllAtomic_aborts_before_flip_when_a_stage_write_fails()
    {
        var d = ScratchDir();
        try
        {
            var a = Path.Combine(d, "a.csv");
            var b = Path.Combine(d, "b.csv");
            File.WriteAllText(a, "old-a");
            File.WriteAllText(b, "old-b");
            // 존재하지 않는 하위 디렉터리 경로 → 세 번째 temp 쓰기가 실패
            var bad = Path.Combine(d, "no_such_dir", "c.csv");

            Assert.ThrowsAny<Exception>(() =>
                TextFileIo.WriteAllAtomic(new (string, byte[])[]
                {
                    (a, Encoding.UTF8.GetBytes("new-a")),
                    (b, Encoding.UTF8.GetBytes("new-b")),
                    (bad, Encoding.UTF8.GetBytes("new-c")),
                }));

            // flip 전에 중단 → 정본은 원본 그대로(부분 상태 0)
            Assert.Equal("old-a", File.ReadAllText(a));
            Assert.Equal("old-b", File.ReadAllText(b));
            Assert.Empty(Directory.GetFiles(d, "*editortmp*"));
        }
        finally { Directory.Delete(d, true); }
    }
}
