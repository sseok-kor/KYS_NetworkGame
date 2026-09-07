using System.IO;

namespace DataEditor.Services;

/// <summary>실행 파일 위치에서 위로 올라가며 리포의 Data/ 폴더를 찾는다(개발 중 자동 로드용).</summary>
public static class DataDirLocator
{
    public static string? Locate()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            var probe = Path.Combine(dir.FullName, "Data", "maps.csv");
            if (File.Exists(probe)) return Path.Combine(dir.FullName, "Data");
            dir = dir.Parent;
        }
        return null;
    }
}
