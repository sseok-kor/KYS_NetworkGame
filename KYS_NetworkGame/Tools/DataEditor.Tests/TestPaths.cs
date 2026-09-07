using DataEditor.Loader;

namespace DataEditor.Tests;

internal static class TestPaths
{
    public static string DataDir()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            var probe = Path.Combine(dir.FullName, "Data", "maps.csv");
            if (File.Exists(probe)) return Path.Combine(dir.FullName, "Data");
            dir = dir.Parent;
        }
        throw new DirectoryNotFoundException("리포 Data/ 폴더를 찾지 못함");
    }

    public static DataLoader LoadAll()
    {
        var loader = new DataLoader();
        loader.LoadAll(DataDir());
        return loader;
    }
}
