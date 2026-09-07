using DataEditor.Core;
using DataEditor.Model;
using static DataEditor.Core.EditorConstants;

namespace DataEditor.Validation;

/// <summary>
/// 게임 부팅 fail-fast 검증의 C# 미러(two-layer 편의층). 부팅 C++ 검증이 권위 SSOT이고
/// 이 엔진은 편집 시점에 같은 규칙을 재현해 즉시 경고한다(설계 §6·§13).
/// 모든 좌표 검사는 폐구간(&lt;=) — 반개구간으로 짜면 게임이 통과시키는 데이터를 오거부(리뷰 강조).
/// 게이트: 현재 Data/(게임 부팅 통과)에 대해 Error 0을 내야 한다.
/// </summary>
public sealed class ValidationEngine
{
    public List<Finding> Validate(Dataset ds)
    {
        var f = new List<Finding>();
        // 로드 순서대로(교차의존): maps -> walkable -> portals -> items -> monsters -> spawns
        ValidateMaps(ds, f);
        ValidatePortals(ds, f);
        ValidateItems(ds, f);
        ValidateMonsters(ds, f);
        ValidateSpawns(ds, f);
        return f;
    }

    private static void Err(List<Finding> f, string cat, object? e, string msg)
        => f.Add(new Finding { Severity = FindingSeverity.Error, Category = cat, Entity = e, Message = msg });
    private static void Warn(List<Finding> f, string cat, object? e, string msg)
        => f.Add(new Finding { Severity = FindingSeverity.Warning, Category = cat, Entity = e, Message = msg });

    private static bool HasBadChar(string s) => s.IndexOf(',') >= 0 || s.IndexOf('\r') >= 0 || s.IndexOf('\n') >= 0;

    private static WalkableGrid? GridFor(Dataset ds, MapData m)
        => m.HasWalkableFile && ds.Walkables.TryGetValue(m.WalkableFile, out var g) ? g : null;

    // ---------------- maps + walkable(맵 참조) ----------------
    private void ValidateMaps(Dataset ds, List<Finding> f)
    {
        if (ds.Maps.Count > MaxMapCount)
            Err(f, "maps", null, $"맵 개수 {ds.Maps.Count} > 상한 {MaxMapCount}");

        long capSum = 0;
        for (int i = 0; i < ds.Maps.Count; i++)
        {
            var m = ds.Maps[i];
            if (m.MapId != i) Err(f, "maps", m, $"mapId {m.MapId} != 행 인덱스 {i} (0부터 연속)");
            if (m.Name.Length > MapNameMaxChars) Err(f, "maps", m, $"name 길이 {m.Name.Length} > {MapNameMaxChars}");
            if (HasBadChar(m.Name)) Err(f, "maps", m, "name에 콤마/개행 포함 금지");
            if (m.WalkableFile.Length > MapFileMaxChars) Err(f, "maps", m, $"walkableFile 길이 > {MapFileMaxChars}");
            if (HasBadChar(m.WalkableFile)) Err(f, "maps", m, "walkableFile에 콤마/개행 포함 금지");
            if (!SizeOk(m.Width)) Err(f, "maps", m, $"width {m.Width} 는 [{MinMapSizePx},{MaxMapSizePx}]·{MapSizeStepPx}배수여야 함");
            if (!SizeOk(m.Height)) Err(f, "maps", m, $"height {m.Height} 는 [{MinMapSizePx},{MaxMapSizePx}]·{MapSizeStepPx}배수여야 함");
            if (m.MonsterCap < 0) Err(f, "maps", m, $"monsterCap {m.MonsterCap} < 0");
            // respawn 폐구간 [0,width]x[0,height]
            if (m.RespawnX < 0 || m.RespawnX > m.Width || m.RespawnY < 0 || m.RespawnY > m.Height)
                Err(f, "maps", m, $"respawn({m.RespawnX},{m.RespawnY})이 맵 폐구간 밖");
            // walkableFile: 정확히 "-" 만 전-통행 맵. 그 외(빈 문자열 포함)는 로드 가능한 파일이어야 함(리뷰 F4).
            //   게임은 "-" 만 벽-없음으로 취급하고 나머지(빈 문자열도)는 파일로 열려다 실패 -> 부팅 거부.
            if (m.WalkableFile != "-")
            {
                if (string.IsNullOrWhiteSpace(m.WalkableFile))
                    Err(f, "walkable", m, "walkableFile이 비어 있음 (전-통행 맵은 정확히 '-' 여야 함)");
                else if (!ds.Walkables.ContainsKey(m.WalkableFile))
                    Err(f, "walkable", m, $"walkableFile '{m.WalkableFile}' 파일을 찾을 수 없음");
            }
            // walkable 격자 = 행 수 + 행별 길이 + 문자('0'/'1') 전수 검증(게임 ParseWalkCsv 미러·리뷰 F1/F5)
            var grid = GridFor(ds, m);
            if (grid != null)
            {
                int expCols = m.Width / WalkCellSize;
                int expRows = m.Height / WalkCellSize;
                if (grid.Rows != expRows)
                    Err(f, "walkable", m, $"통행 행 수 {grid.Rows} != {expRows} (맵 높이/50)");
                // 게임은 첫 위반 행에서 중단(fail-fast) → 우리도 첫 위반만 보고(findings 폭주 방지)
                for (int r = 0; r < grid.Rows; r++)
                {
                    if (grid.RowHasBadChar(r)) { Err(f, "walkable", m, $"통행 {r}행에 '0'/'1' 이 아닌 문자 포함"); break; }
                    if (grid.RowLength(r) != expCols) { Err(f, "walkable", m, $"통행 {r}행 길이 {grid.RowLength(r)} != {expCols} (맵 폭/50)"); break; }
                }
            }
            // respawn 통행 가능
            if (!PortedGeo.IsWalkable(grid, m.Width, m.Height, m.RespawnX, m.RespawnY))
                Err(f, "maps", m, "respawn이 벽 위");

            capSum += m.MonsterCap;
        }
        if (capSum > MonsterPoolCapacity)
            Err(f, "maps", null, $"전맵 monsterCap 합 {capSum} > {MonsterPoolCapacity}");
    }

    private static bool SizeOk(int px) => px >= MinMapSizePx && px <= MaxMapSizePx && px % MapSizeStepPx == 0;

    // ---------------- portals(교차) ----------------
    private void ValidatePortals(Dataset ds, List<Finding> f)
    {
        if (ds.Portals.Count > MaxPortals) // 관대: 게임은 절단·부팅 통과(리뷰 #4)
            Warn(f, "portals", null, $"포탈 개수 {ds.Portals.Count} > {MaxPortals} (초과분은 게임이 무시)");

        int mapCount = ds.Maps.Count;
        foreach (var p in ds.Portals)
        {
            if (p.SrcMapId < 0 || p.SrcMapId >= mapCount) { Err(f, "portals", p, $"srcMapId {p.SrcMapId} 범위 밖"); continue; }
            if (p.DstMapId < 0 || p.DstMapId >= mapCount) { Err(f, "portals", p, $"dstMapId {p.DstMapId} 범위 밖"); continue; }
            var src = ds.Maps[p.SrcMapId];
            var dst = ds.Maps[p.DstMapId];
            var srcGrid = GridFor(ds, src);
            var dstGrid = GridFor(ds, dst);

            if (p.TriggerX < 0 || p.TriggerX > src.Width || p.TriggerY < 0 || p.TriggerY > src.Height)
                Err(f, "portals", p, "트리거가 src 맵 폐구간 밖");
            else if (!PortedGeo.IsWalkable(srcGrid, src.Width, src.Height, p.TriggerX, p.TriggerY))
                Err(f, "portals", p, "트리거가 벽 위");
            else if (!PortedGeo.IsReachable(srcGrid, src.RespawnX, src.RespawnY, p.TriggerX, p.TriggerY))
                Err(f, "portals", p, "트리거가 src respawn에서 도달 불가(벽 격리)");

            if (p.DstX < 0 || p.DstX > dst.Width || p.DstY < 0 || p.DstY > dst.Height)
                Err(f, "portals", p, "도착점이 dst 맵 폐구간 밖");
            else if (!PortedGeo.IsWalkable(dstGrid, dst.Width, dst.Height, p.DstX, p.DstY))
                Err(f, "portals", p, "도착점이 벽 위");
            else if (!PortedGeo.IsReachable(dstGrid, dst.RespawnX, dst.RespawnY, p.DstX, p.DstY))
                Err(f, "portals", p, "도착점이 dst respawn에서 도달 불가(도착해도 갇힘)");
        }
    }

    // ---------------- items(관대·에디터는 FK 무결성 엄격) ----------------
    private void ValidateItems(Dataset ds, List<Finding> f)
    {
        if (ds.Items.Count > MaxItemDefs) // 관대: 게임 절단·부팅 통과(리뷰 #4)
            Warn(f, "items", null, $"아이템 개수 {ds.Items.Count} > {MaxItemDefs} (초과분은 게임이 무시)");

        var seen = new HashSet<int>();
        foreach (var it in ds.Items)
        {
            if (HasBadChar(it.Name)) Err(f, "items", it, "name에 콤마/개행 포함 금지"); // 게임 컬럼 밀림 → 유령 아이템(리뷰 #9)
            // 유령 드랍 방지 위해 에디터는 유일성·기본 정합을 Error로 강제(설계 §6)
            if (it.TemplateId <= 0) Err(f, "items", it, $"templateId {it.TemplateId} 는 0 초과여야 함");
            else if (!seen.Add(it.TemplateId)) Err(f, "items", it, $"templateId {it.TemplateId} 중복");
            if (it.Type < 0 || it.Type >= ItemTypeCount) Err(f, "items", it, $"type {it.Type} 범위 밖(0..{ItemTypeCount - 1})");
            if (it.StackMax <= 0) Err(f, "items", it, $"stackMax {it.StackMax} 는 0 초과여야 함");
        }
    }

    private static readonly string[] RequiredMonsterKeys =
        { "type", "hp", "atkPower", "moveSpeed", "aggroRange", "attackRange", "attackCooldown" };

    // ---------------- monsters(items FK 참조) ----------------
    private void ValidateMonsters(Dataset ds, List<Finding> f)
    {
        // 게임 아이템 테이블 절단 미러(리뷰 F6): 첫 128개 "유효 수용" 아이템만 조회 가능(그 뒤 전부 무시).
        //   수용 조건 = type 범위 + templateId>0 + stackMax>0 + templateId 중복 아님(첫 행 우선). 128 도달 시 loop 중단.
        var resolvable = new Dictionary<int, ItemDef>();
        foreach (var it in ds.Items)
        {
            if (resolvable.Count >= MaxItemDefs) break;
            if (it.Type < 0 || it.Type >= ItemTypeCount) continue;
            if (it.TemplateId <= 0 || it.StackMax <= 0) continue;
            if (!resolvable.ContainsKey(it.TemplateId)) resolvable.Add(it.TemplateId, it);
        }

        var seenType = new HashSet<int>();
        int validDrops = 0; // 게임 dropRuleCount 미러: FK+stackMax+chancePermil+min/max 모두 통과한 드랍만(리뷰 F3/F7)
        foreach (var m in ds.Monsters)
        {
            if (m.Type < 0 || m.Type >= MonsterTypeCount) Err(f, "monsters", m, $"type {m.Type} 범위 밖(0..{MonsterTypeCount - 1})");
            else if (!seenType.Add(m.Type)) Err(f, "monsters", m, $"type {m.Type} 중복");
            if (m.MoveSpeed < MinMoveSpeed || m.MoveSpeed > MaxMoveSpeed)
                Err(f, "monsters", m, $"moveSpeed {m.MoveSpeed} 는 [{MinMoveSpeed},{MaxMoveSpeed}] 이어야 함");
            if (m.Name.Length > MapNameMaxChars) Err(f, "monsters", m, $"name 길이 > {MapNameMaxChars}");
            if (m.IsBoss && m.Cap < 1) Err(f, "monsters", m, "isBoss=true면 cap>=1 이어야 함");

            // 필수 스탯 필드 누락(게임 부팅 거부·YamlDotNet은 0으로 무음 채움·리뷰 F8).
            //   LoadedKeys 는 파일 로드 시만 채워지므로 수동 구성 객체(Count==0)는 present 판별 불가 -> skip.
            if (m.LoadedKeys.Count > 0)
                foreach (var req in RequiredMonsterKeys)
                    if (!m.LoadedKeys.Contains(req))
                        Err(f, "monsters", m, $"필수 스탯 필드 '{req}' 누락 (게임 부팅 거부)");

            foreach (var d in m.Drops)
            {
                // FK/stackMax(Error) 먼저 → chancePermil/min-max(Warning) 나중(게임 평가 순서).
                //   FK 조회는 절단 미러(resolvable) 기준 -> 128 뒤로 밀린 아이템 참조는 유령 드랍 Error.
                resolvable.TryGetValue(d.Item, out var item);
                if (item == null) Err(f, "monsters", m, $"드랍 item {d.Item} 이 items에 없음(유령 드랍)");
                else if (d.Max > item.StackMax) Err(f, "monsters", m, $"드랍 max {d.Max} > 아이템 stackMax {item.StackMax}");

                bool chanceOk = d.ChancePermil >= ChancePermilMin && d.ChancePermil <= ChancePermilMax;
                bool qtyOk = d.Min > 0 && d.Max >= d.Min;
                if (!chanceOk) Warn(f, "monsters", m, $"드랍 chancePermil {d.ChancePermil} 범위 밖 → 게임이 이 드랍을 무시");
                if (!qtyOk) Warn(f, "monsters", m, $"드랍 수량 min={d.Min} max={d.Max} 부적합 → 게임이 이 드랍을 무시");

                if (item != null && d.Max <= item.StackMax && chanceOk && qtyOk) validDrops++;
            }
        }
        // 전 9종 정의 필수
        for (int t = 0; t < MonsterTypeCount; t++)
            if (!seenType.Contains(t)) Err(f, "monsters", null, $"몬스터 type {t} 미정의(0..{MonsterTypeCount - 1} 전수 필요)");
        if (validDrops <= 0) Err(f, "monsters", null, "유효 드랍 규칙이 0행(>=1 필요·게임 부팅 거부)");
        if (validDrops > MaxDropRules) Err(f, "monsters", null, $"유효 드랍 규칙 {validDrops} > {MaxDropRules} (게임 저장소 초과 부팅 거부)");
    }

    // ---------------- spawns(maps+monsters+walkable 교차) ----------------
    private void ValidateSpawns(Dataset ds, List<Finding> f)
    {
        if (ds.Spawns.Count > MaxSpawnGroups) Err(f, "spawns", null, $"스폰 그룹 {ds.Spawns.Count} > {MaxSpawnGroups}");
        int mapCount = ds.Maps.Count;

        // per-map count 합, per-map-per-type count 합(리뷰 #5)
        var mapTotal = new Dictionary<int, long>();
        var mapTypeTotal = new Dictionary<(int map, int type), long>();
        long grandTotal = 0;

        foreach (var s in ds.Spawns)
        {
            if (s.MapId < 0 || s.MapId >= mapCount) { Err(f, "spawns", s, $"mapId {s.MapId} 범위 밖"); continue; }
            if (s.Type < 0 || s.Type >= MonsterTypeCount) Err(f, "spawns", s, $"type {s.Type} 범위 밖");
            if (s.X0 > s.X1 || s.Y0 > s.Y1) Err(f, "spawns", s, "구역 좌표 역전(x0<=x1, y0<=y1)");
            if (s.Count < 1 || s.Count > MaxGroupCount) Err(f, "spawns", s, $"count {s.Count} 는 [1,{MaxGroupCount}]");
            if (s.RespawnDelayMs < 1) Err(f, "spawns", s, $"respawnDelayMs {s.RespawnDelayMs} 는 >=1 (0은 구버전 파일)");

            var map = ds.Maps[s.MapId];
            var grid = GridFor(ds, map);
            // 구역이 맵 폐구간 안
            if (s.X0 < 0 || s.X1 > map.Width || s.Y0 < 0 || s.Y1 > map.Height)
                Err(f, "spawns", s, "스폰 구역이 맵 밖");
            else
            {
                // 어그로 거리: 구역-respawn 최단거리 >= aggroRange
                var mon = ds.FindMonster(s.Type);
                int aggro = mon?.AggroRange ?? 0;
                long distSq = PortedGeo.RectPointDistSq(s.X0, s.Y0, s.X1, s.Y1, map.RespawnX, map.RespawnY);
                if (distSq < (long)aggro * aggro)
                    Err(f, "spawns", s, $"구역이 respawn에서 어그로거리({aggro}) 안 → 부활 즉시 피격");

                // 구역 통행칸 존재 + 전 통행칸 도달성
                if (grid != null)
                {
                    bool anyWalkable = false;
                    bool allReachable = true;
                    foreach (var (sx, sy) in PortedGeo.ZoneSamplePoints(s.X0, s.Y0, s.X1, s.Y1, grid.Cols, grid.Rows))
                    {
                        if (!PortedGeo.IsWalkable(grid, map.Width, map.Height, sx, sy)) continue;
                        anyWalkable = true;
                        if (!PortedGeo.IsReachable(grid, map.RespawnX, map.RespawnY, sx, sy)) { allReachable = false; break; }
                    }
                    if (!anyWalkable) Err(f, "spawns", s, "구역에 통행 칸이 하나도 없음(스폰 불가)");
                    else if (!allReachable) Err(f, "spawns", s, "구역에 respawn에서 도달 불가한 통행 칸(갇힌 섬)");
                }
            }

            if (s.MapId >= 0 && s.MapId < mapCount && s.Type >= 0 && s.Type < MonsterTypeCount)
            {
                mapTotal.TryGetValue(s.MapId, out var mt); mapTotal[s.MapId] = mt + s.Count;
                var key = (s.MapId, s.Type);
                mapTypeTotal.TryGetValue(key, out var tt); mapTypeTotal[key] = tt + s.Count;
                grandTotal += s.Count;
            }
        }

        // per-map count 합 <= monsterCap
        foreach (var kv in mapTotal)
        {
            var map = ds.Maps[kv.Key];
            if (kv.Value > map.MonsterCap)
                Err(f, "spawns", map, $"맵 {kv.Key} 스폰 합 {kv.Value} > monsterCap {map.MonsterCap}");
        }
        // per-map-per-type <= cap(cap>0)
        foreach (var kv in mapTypeTotal)
        {
            var mon = ds.FindMonster(kv.Key.type);
            int cap = mon?.Cap ?? 0;
            if (cap > 0 && kv.Value > cap)
                Err(f, "spawns", mon, $"맵 {kv.Key.map} 의 type {kv.Key.type} 스폰 합 {kv.Value} > cap {cap}");
        }
        if (grandTotal > MonsterPoolCapacity)
            Err(f, "spawns", null, $"전맵 스폰 합 {grandTotal} > {MonsterPoolCapacity}");
    }
}
