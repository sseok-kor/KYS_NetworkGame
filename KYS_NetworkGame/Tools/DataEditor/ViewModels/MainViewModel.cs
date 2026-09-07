using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using DataEditor.Edit;
using DataEditor.Loader;
using DataEditor.Model;
using DataEditor.Services;
using DataEditor.Validation;

namespace DataEditor.ViewModels;

/// <summary>
/// 메인 ViewModel. Data/를 로드해 6종 컬렉션 + 검증 결과를 노출하고, 편집 시 라이브 재검증한다.
/// (표 데이터 편집 + 공간 캔버스 편집 + 검증 + 저장. 두 편집 경로가 같은 undo 스택(History)에 얹힌다.)
/// </summary>
public partial class MainViewModel : ObservableObject
{
    private DataLoader? _loader;
    private readonly ValidationEngine _validator = new();

    /// <summary>캔버스 탭 인덱스(MainWindow 탭 순서: Maps..Items=0..4, 캔버스=5).</summary>
    public const int CanvasTabIndex = 5;

    public CommandHistory History { get; } = new();

    public ObservableCollection<MapData> Maps { get; } = new();
    public ObservableCollection<SpawnGroup> Spawns { get; } = new();
    public ObservableCollection<Portal> Portals { get; } = new();
    public ObservableCollection<MonsterTemplate> Monsters { get; } = new();
    public ObservableCollection<ItemDef> Items { get; } = new();
    public ObservableCollection<Finding> Findings { get; } = new();

    [ObservableProperty] private string _status = "준비";
    [ObservableProperty] private string? _dataDir;
    [ObservableProperty] private int _errorCount;
    [ObservableProperty] private int _warningCount;

    /// <summary>캔버스가 참조하는 전체 데이터셋(스폰/포탈을 맵별로 조회).</summary>
    public Dataset? Dataset => _loader?.Dataset;

    /// <summary>캔버스에 표시할 선택 맵.</summary>
    [ObservableProperty] private MapData? _selectedMap;

    /// <summary>탭 선택 인덱스(목록 더블클릭 시 캔버스 탭으로 이동).</summary>
    [ObservableProperty] private int _selectedTabIndex;

    /// <summary>캔버스에서 하이라이트할 엔티티(목록에서 넘어온 스폰/포탈).</summary>
    [ObservableProperty] private object? _focusedEntity;

    /// <summary>Monsters 탭에서 선택한 몬스터 — 그 드랍 테이블을 하위 그리드에 표시.</summary>
    [ObservableProperty] private MonsterTemplate? _selectedMonster;

    /// <summary>캔버스 편집 도구: 0=통행 페인트, 1=선택·이동(스폰/포탈 드래그).</summary>
    [ObservableProperty] private int _canvasTool;

    /// <summary>검증에서 오류/경고가 걸린 엔티티 집합(캔버스가 경고 오버레이로 표시). 참조 동일성 기준.</summary>
    [ObservableProperty] private HashSet<object> _flaggedEntities = new(ReferenceEqualityComparer.Instance);

    /// <summary>버튼/컨텍스트 메뉴 "캔버스에서 보기" — 선택 엔티티를 캔버스로(더블클릭 셀편집 충돌 회피용 대체 제스처).</summary>
    [RelayCommand]
    private void ViewOnCanvas(object? entity) => NavigateTo(entity);

    /// <summary>목록 엔티티 → 그 맵의 캔버스로 이동 + 하이라이트(엔티티→공간 포커스).</summary>
    public void NavigateTo(object? entity)
    {
        MapData? map = entity switch
        {
            MapData m => m,
            SpawnGroup s => Dataset?.FindMap(s.MapId),
            Portal p => Dataset?.FindMap(p.SrcMapId),
            _ => null,
        };
        if (map == null) return;
        SelectedMap = map;
        FocusedEntity = entity is MapData ? null : entity;
        SelectedTabIndex = CanvasTabIndex;
    }

    public MainViewModel()
    {
        var dir = DataDirLocator.Locate();
        if (dir != null) LoadFrom(dir);
        else Status = "Data/ 폴더를 찾지 못함 — [Load]로 지정하세요";
    }

    [RelayCommand]
    private void Load()
    {
        var dir = DataDirLocator.Locate();
        if (dir == null) { Status = "Data/ 폴더를 찾지 못함"; return; }
        LoadFrom(dir);
    }

    private void LoadFrom(string dir)
    {
        _loader = new DataLoader();
        _loader.LoadAll(dir);
        DataDir = dir;

        Fill(Maps, _loader.Dataset.Maps);
        Fill(Spawns, _loader.Dataset.Spawns);
        Fill(Portals, _loader.Dataset.Portals);
        Fill(Monsters, _loader.Dataset.Monsters);
        Fill(Items, _loader.Dataset.Items);

        // 편집 시 라이브 재검증(two-layer 편의 검증의 핵심 — 편집 즉시 오류 피드백)
        SubscribeRevalidate(Maps);
        SubscribeRevalidate(Spawns);
        SubscribeRevalidate(Portals);
        SubscribeRevalidate(Monsters);
        SubscribeRevalidate(Items);
        // 드랍(중첩 컬렉션)도 편집 시 재검증
        foreach (var mon in Monsters)
            foreach (var d in mon.Drops)
                d.PropertyChanged += (_, __) => Validate();

        Validate();
        OnPropertyChanged(nameof(Dataset));
        SelectedMap = Maps.FirstOrDefault();
        SelectedMonster = Monsters.FirstOrDefault();
        Status = $"로드 완료: {dir}";
    }

    private static void Fill<T>(ObservableCollection<T> dst, IEnumerable<T> src)
    {
        dst.Clear();
        foreach (var x in src) dst.Add(x);
    }

    private void SubscribeRevalidate<T>(ObservableCollection<T> items) where T : INotifyPropertyChanged
    {
        foreach (var it in items) it.PropertyChanged += (_, __) => Validate();
    }

    [RelayCommand]
    private void Validate()
    {
        if (_loader == null) return;
        var findings = _validator.Validate(_loader.Dataset);
        Fill(Findings, findings);
        ErrorCount = 0;
        WarningCount = 0;
        var flagged = new HashSet<object>(ReferenceEqualityComparer.Instance);
        foreach (var f in findings)
        {
            if (f.Severity == FindingSeverity.Error) ErrorCount++;
            else WarningCount++;
            if (f.Entity != null) flagged.Add(f.Entity);
        }
        FlaggedEntities = flagged; // 새 참조 → 캔버스 재렌더 트리거
        Status = ErrorCount == 0
            ? $"검증 통과 (경고 {WarningCount})"
            : $"오류 {ErrorCount} · 경고 {WarningCount} — 서버가 부팅 거부할 수 있음";
    }

    [RelayCommand]
    private void Undo() { History.Undo(); Validate(); }

    [RelayCommand]
    private void Redo() { History.Redo(); Validate(); }

    [RelayCommand]
    private void Save()
    {
        if (_loader == null || DataDir == null) return;
        Validate();

        // 다파일 stage-all-then-flip 원자 커밋(설계 §13 #2) — 모든 temp 를 먼저 쓰고 전부 성공 시에만 뒤집는다.
        var d = DataDir;
        var files = new List<(string, byte[])>
        {
            (Path.Combine(d, "maps.csv"), _loader.MapsTable!.SaveBytes()),
            (Path.Combine(d, "spawns.csv"), _loader.SpawnsTable!.SaveBytes()),
            (Path.Combine(d, "portals.csv"), _loader.PortalsTable!.SaveBytes()),
            (Path.Combine(d, "items.csv"), _loader.ItemsTable!.SaveBytes()),
        };
        if (_loader.MonstersFile != null)
            files.Add((Path.Combine(d, "monsters.yml"), _loader.MonstersFile.SaveBytes()));

        // walkable CSV 를 쓰면 짝 .bin 캐시를 지운다(게임은 mtime 불일치로도 무효화하나 명시 삭제로 확실히·리뷰 #11).
        var deleteBins = new List<string>();
        foreach (var kv in _loader.WalkableFiles)
        {
            files.Add((Path.Combine(d, kv.Key), kv.Value.SaveBytes()));
            deleteBins.Add(Path.Combine(d, TextFileIo.WalkableBinName(kv.Key)));
        }

        try
        {
            TextFileIo.WriteAllAtomic(files, deleteBins);
        }
        catch (PartialCommitException pex)
        {
            Status = $"저장 부분 실패 — {pex.Committed}/{pex.Total} 파일만 갱신됨. 데이터 확인 후 재저장 필요";
            return;
        }
        catch (Exception ex)
        {
            Status = $"저장 실패 — 원본 보존됨: {ex.Message}";
            return;
        }

        Status = ErrorCount == 0 ? "저장 완료" : $"저장 완료 (⚠ 오류 {ErrorCount} — 서버 부팅 거부 가능)";
    }

    // ---- 엔티티 추가/삭제(CSV 백엔드: spawns/portals/items) ----
    //   Attach/Detach 가역 프리미티브로 CsvTable(줄+매핑) + Dataset + vm 3리스트를 함께 동기화하고
    //   DelegateEditAction 으로 표·캔버스와 같은 undo 스택에 얹는다(엔티티 add/remove 도 Ctrl+Z 대상).
    //   monsters(9종 불변식)·maps(DB map_id 참조)는 v1 add/remove 제외. drops(yml 중첩)는 아래 AddDrop/RemoveDrop 이 별도 경로로 처리.
    private void AddCsvEntity<T>(CsvTable<T>? table, ObservableCollection<T> dsColl, ObservableCollection<T> vmColl, T entity)
        where T : INotifyPropertyChanged
    {
        if (table == null) return;
        entity.PropertyChanged += (_, __) => Validate(); // 새 엔티티 편집도 라이브 재검증
        void Attach() { table.Attach(entity); dsColl.Add(entity); vmColl.Add(entity); }
        void Detach() { table.Detach(entity); dsColl.Remove(entity); vmColl.Remove(entity); }
        History.Do(new DelegateEditAction(Attach, Detach)); // Do 가 Attach 실행(3리스트+doc 줄 추가)
        Validate();
    }

    private void RemoveCsvEntity<T>(CsvTable<T>? table, ObservableCollection<T> dsColl, ObservableCollection<T> vmColl, T entity)
        where T : INotifyPropertyChanged
    {
        if (table == null) return;
        void Attach() { table.Attach(entity); dsColl.Add(entity); vmColl.Add(entity); }
        void Detach() { table.Detach(entity); dsColl.Remove(entity); vmColl.Remove(entity); }
        History.Do(new DelegateEditAction(Detach, Attach)); // Do 가 Detach 실행(삭제)
        Validate();
    }

    [RelayCommand]
    private void AddSpawn()
    {
        if (_loader == null) return;
        var e = new SpawnGroup { MapId = SelectedMap?.MapId ?? 0, Type = 0, X0 = 0, Y0 = 0, X1 = 100, Y1 = 100, Count = 1, RespawnDelayMs = 5000 };
        AddCsvEntity(_loader.SpawnsTable, _loader.Dataset.Spawns, Spawns, e);
    }

    [RelayCommand]
    private void RemoveSpawn(SpawnGroup? s)
    {
        if (_loader != null && s != null) RemoveCsvEntity(_loader.SpawnsTable, _loader.Dataset.Spawns, Spawns, s);
    }

    [RelayCommand]
    private void AddPortal()
    {
        if (_loader == null) return;
        var e = new Portal { SrcMapId = SelectedMap?.MapId ?? 0, TriggerX = 0, TriggerY = 0, TriggerRadius = 50, DstMapId = 0, DstX = 0, DstY = 0 };
        AddCsvEntity(_loader.PortalsTable, _loader.Dataset.Portals, Portals, e);
    }

    [RelayCommand]
    private void RemovePortal(Portal? p)
    {
        if (_loader != null && p != null) RemoveCsvEntity(_loader.PortalsTable, _loader.Dataset.Portals, Portals, p);
    }

    [RelayCommand]
    private void AddItem()
    {
        if (_loader == null) return;
        int nextId = Items.Count > 0 ? Items.Max(i => i.TemplateId) + 1 : 1001;
        var e = new ItemDef { TemplateId = nextId, Name = "새아이템", Type = 0, StackMax = 1 };
        AddCsvEntity(_loader.ItemsTable, _loader.Dataset.Items, Items, e);
    }

    [RelayCommand]
    private void RemoveItem(ItemDef? it)
    {
        if (_loader != null && it != null) RemoveCsvEntity(_loader.ItemsTable, _loader.Dataset.Items, Items, it);
    }

    // ---- 드랍 추가/삭제(monsters.yml 중첩) — MonstersYaml AttachDrop/DetachDrop + 모델 Drops + undo 통합 ----
    [RelayCommand]
    private void AddDrop()
    {
        var m = SelectedMonster;
        if (_loader?.MonstersFile == null || m == null) return;
        int mi = Monsters.IndexOf(m);
        if (mi < 0) return;
        var yaml = _loader.MonstersFile;
        var drop = new DropRule { Item = Items.Count > 0 ? Items[0].TemplateId : 1001, ChancePermil = 100, Min = 1, Max = 1 };
        drop.PropertyChanged += (_, __) => Validate();
        void Attach() { m.Drops.Add(drop); yaml.AttachDrop(mi, drop); }
        void Detach() { m.Drops.Remove(drop); yaml.DetachDrop(drop); }
        History.Do(new DelegateEditAction(Attach, Detach));
        Validate();
    }

    [RelayCommand]
    private void RemoveDrop(DropRule? drop)
    {
        var m = SelectedMonster;
        if (_loader?.MonstersFile == null || m == null || drop == null) return;
        int mi = Monsters.IndexOf(m);
        if (mi < 0) return;
        var yaml = _loader.MonstersFile;
        void Attach() { m.Drops.Add(drop); yaml.AttachDrop(mi, drop); }
        void Detach() { m.Drops.Remove(drop); yaml.DetachDrop(drop); }
        History.Do(new DelegateEditAction(Detach, Attach));
        Validate();
    }
}
