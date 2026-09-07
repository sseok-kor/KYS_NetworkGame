using System.Globalization;
using DataEditor.Model;
using YamlDotNet.Serialization;
using YamlDotNet.Serialization.NamingConventions;

namespace DataEditor.Loader;

/// <summary>
/// monsters.yml 로더. 읽기는 YamlDotNet(견고한 파서), 쓰기는 줄 단위 표면 편집(리뷰 #1).
/// serializer round-trip은 주석·flow 스타일·1.0 float를 정규화해 바이트 동등이 불가하므로,
/// 편집 안 된 줄은 원문 바이트 그대로 흘리고, 편집된 스칼라/드랍 줄만 그 자리에서 값을 교체한다.
/// → 미편집 저장 바이트 동등 + 편집 반영 둘 다 달성.
/// 전제: 파일이 우리 표준 스타일(2공백 몬스터·4공백 필드·6공백 flow 드랍)을 따른다.
/// 알려진 한계(F10·표준 스타일 밖): 값 뒤 인라인 주석("hp: 50   # tanky")이 있는 줄을 "편집"하면
///   그 줄만 값까지로 재직렬화되어 인라인 주석이 사라진다(미편집 줄·전체행 주석은 원문 보존). 표준 데이터엔 없음.
/// </summary>
public sealed class MonstersYaml
{
    private readonly byte[]? _bom;
    private readonly string _newline;
    private readonly List<ILine> _lines;

    public List<MonsterTemplate> Monsters { get; }

    private MonstersYaml(byte[]? bom, string newline, List<ILine> lines, List<MonsterTemplate> monsters)
    {
        _bom = bom;
        _newline = newline;
        _lines = lines;
        Monsters = monsters;
    }

    private interface ILine { string Emit(); }
    private sealed class RawLine : ILine
    {
        public string Raw = "";
        public string Emit() => Raw;
    }
    // "    hp: 50" 같은 스칼라 필드 줄
    private sealed class FieldLine : ILine
    {
        public string Raw = "";
        public string Prefix = "";      // "    hp: " (콜론+공백까지)
        public bool Dirty;
        public Func<string> Value = () => "";
        public string Emit() => Dirty ? Prefix + Value() : Raw;
    }
    // "      - { item: 1001, chancePermil: 300, min: 1, max: 2 }" 드랍 줄
    private sealed class DropLine : ILine
    {
        public string Raw = "";
        public string Prefix = "";      // "      - "
        public bool Dirty;
        public int MonsterIndex = -1;   // 소유 몬스터(드랍 추가 시 삽입 지점 판별)
        public DropRule? Drop;          // 이 줄이 나타내는 드랍(삭제 시 참조로 찾음)
        public Func<string> Flow = () => "";
        public string Emit() => Dirty ? Prefix + Flow() : Raw;
    }
    // "    drops:" 헤더 줄 — 드랍이 0개인 몬스터에 첫 드랍을 추가할 때 삽입 지점.
    private sealed class DropsHeaderLine : ILine
    {
        public string Raw = "";
        public int MonsterIndex = -1;
        public string Emit() => Raw;
    }
    // 파일에 물리적 줄이 없던 필드가 편집되면 활성화되어 그 몬스터 블록 끝(마지막 스칼라 줄 뒤)에 새 줄로 삽입된다(F9).
    // 비활성(미편집)이면 SaveBytes가 아예 제외 → 미편집 저장 바이트 동등 유지.
    private sealed class InsertedLine : ILine
    {
        public bool Active;
        public string Key = "";
        public Func<string> Value = () => "";
        public string Emit() => $"    {Key}: {Value()}";  // 표준 스타일 = 4공백 스칼라 필드
    }

    // 몬스터 블록의 직렬화 가능한 스칼라 키 전체(F9 삽입 후보 = 이 중 파일에 줄 없는 것).
    private static readonly string[] AllScalarKeys =
    {
        "name", "type", "hp", "atkPower", "moveSpeed",
        "aggroRange", "attackRange", "attackCooldown", "isBoss", "cap",
    };

    // 프로퍼티명(PascalCase) → yaml 키(camelCase). 예: "AtkPower" -> "atkPower".
    private static string PropToKey(string prop)
        => prop.Length == 0 ? prop : char.ToLowerInvariant(prop[0]) + prop.Substring(1);

    // 파일에 줄이 없던 필드용 삽입 자리표시 생성 — 그 키의 프로퍼티가 바뀌면 활성화(편집 시에만 저장에 등장).
    private static InsertedLine MakeInsert(MonsterTemplate m, string key)
    {
        var ins = new InsertedLine { Key = key, Value = () => FieldValue(m, key) };
        m.PropertyChanged += (_, e) => { if (e.PropertyName != null && PropToKey(e.PropertyName) == key) ins.Active = true; };
        return ins;
    }

    private static string FieldValue(MonsterTemplate m, string key) => key switch
    {
        "name" => $"\"{m.Name}\"",
        "type" => m.Type.ToString(CultureInfo.InvariantCulture),
        "hp" => m.Hp.ToString(CultureInfo.InvariantCulture),
        "atkPower" => m.AtkPower.ToString(CultureInfo.InvariantCulture),
        "moveSpeed" => m.MoveSpeed.ToString(CultureInfo.InvariantCulture),
        "aggroRange" => m.AggroRange.ToString(CultureInfo.InvariantCulture),
        "attackRange" => m.AttackRange.ToString(CultureInfo.InvariantCulture),
        "attackCooldown" => m.AttackCooldown.ToString("0.0##", CultureInfo.InvariantCulture), // "1.0" 포맷 보존
        "isBoss" => m.IsBoss ? "true" : "false",
        "cap" => m.Cap.ToString(CultureInfo.InvariantCulture),
        _ => "",
    };

    private static string DropFlow(DropRule d)
        => $"{{ item: {d.Item}, chancePermil: {d.ChancePermil}, min: {d.Min}, max: {d.Max} }}";

    public static MonstersYaml Load(string path)
    {
        var (text, bom, newline) = TextFileIo.ReadText(path);

        // 1) 모델은 YamlDotNet으로 견고하게 파싱
        var monsters = ParseModel(text);

        // 2) 줄 단위 round-trip 구조 + 모델 연결
        var lines = new List<ILine>();
        int monsterIdx = -1;
        int dropIdx = -1;
        bool inDrops = false;
        var lastScalarIdx = new List<int>();  // 몬스터별 마지막 스칼라 필드 줄의 lines 인덱스(F9 삽입 지점)

        foreach (var raw in text.Split(newline))
        {
            var trimmed = raw.TrimStart();
            int lead = raw.Length - trimmed.Length;

            if (trimmed.Length == 0 || trimmed.StartsWith('#') || lead == 0)
            {
                lines.Add(new RawLine { Raw = raw });
                continue;
            }
            if (lead == 2 && trimmed.StartsWith("- ")) // 새 몬스터 시작(첫 필드)
            {
                monsterIdx++;
                dropIdx = -1;
                inDrops = false;
                lines.Add(MakeField(raw, monsterIdx, monsters));
                lastScalarIdx.Add(lines.Count - 1);
                continue;
            }
            if (lead == 4)
            {
                if (trimmed == "drops:") { inDrops = true; lines.Add(new DropsHeaderLine { Raw = raw, MonsterIndex = monsterIdx }); continue; }
                inDrops = false;
                lines.Add(MakeField(raw, monsterIdx, monsters));
                if (monsterIdx >= 0 && monsterIdx < lastScalarIdx.Count) lastScalarIdx[monsterIdx] = lines.Count - 1;
                continue;
            }
            if (lead == 6 && inDrops && trimmed.StartsWith("- ")) // 드랍 flow 줄
            {
                dropIdx++;
                lines.Add(MakeDrop(raw, monsterIdx, dropIdx, monsters));
                continue;
            }
            lines.Add(new RawLine { Raw = raw });
        }

        // 파일에 줄이 없던 필드용 삽입 자리표시(비활성) — 편집 시에만 활성화되어 저장에 등장(F9).
        //   마지막 스칼라 줄 뒤(drops:/다음 몬스터 앞)에 꽂는다. 뒤에서 앞으로 삽입해 인덱스 밀림 방지.
        for (int mi = monsters.Count - 1; mi >= 0; mi--)
        {
            if (mi >= lastScalarIdx.Count) continue;
            int at = lastScalarIdx[mi] + 1;
            foreach (var key in AllScalarKeys)
            {
                if (monsters[mi].LoadedKeys.Contains(key)) continue; // 이미 물리 줄 있음(FieldLine이 담당)
                lines.Insert(at, MakeInsert(monsters[mi], key));
                at++;
            }
        }

        return new MonstersYaml(bom, newline, lines, monsters);
    }

    private static FieldLine MakeField(string raw, int monsterIdx, List<MonsterTemplate> monsters)
    {
        // prefix = 콜론+공백까지. 실패 시 raw 고정(안전).
        int colon = raw.IndexOf(": ", StringComparison.Ordinal);
        var line = new FieldLine { Raw = raw };
        if (colon < 0 || monsterIdx < 0 || monsterIdx >= monsters.Count) return line;

        line.Prefix = raw.Substring(0, colon + 2);
        // 키 추출: prefix에서 앞 공백/"- " 제거하고 콜론 앞까지
        var keyPart = raw.Substring(0, colon).TrimStart();
        if (keyPart.StartsWith("- ")) keyPart = keyPart.Substring(2);
        var key = keyPart;
        var m = monsters[monsterIdx];
        m.LoadedKeys.Add(key); // 이 몬스터 블록에 물리적으로 존재한 키(F8 누락검증 + F9 삽입 대상 판별)
        line.Value = () => FieldValue(m, key);
        m.PropertyChanged += (_, e) => { if (e.PropertyName != null && PropToKey(e.PropertyName) == key) line.Dirty = true; };
        return line;
    }

    private static DropLine MakeDrop(string raw, int monsterIdx, int dropIdx, List<MonsterTemplate> monsters)
    {
        var line = new DropLine { Raw = raw };
        int dash = raw.IndexOf("- ", StringComparison.Ordinal);
        if (dash < 0 || monsterIdx < 0 || monsterIdx >= monsters.Count) return line;
        var m = monsters[monsterIdx];
        if (dropIdx < 0 || dropIdx >= m.Drops.Count) return line;

        line.Prefix = raw.Substring(0, dash + 2);
        var drop = m.Drops[dropIdx];
        line.MonsterIndex = monsterIdx;
        line.Drop = drop;
        line.Flow = () => DropFlow(drop);
        drop.PropertyChanged += (_, __) => line.Dirty = true;
        return line;
    }

    /// <summary>드랍 추가: 그 몬스터의 마지막 드랍 줄 뒤(없으면 drops: 헤더 뒤)에 새 dirty 드랍 줄을 삽입.
    /// drops 블록이 아예 없으면(현 데이터엔 없음) 무시. Detach로 되돌림.</summary>
    public void AttachDrop(int monsterIdx, DropRule drop)
    {
        int at = -1;
        for (int i = 0; i < _lines.Count; i++)
            if (_lines[i] is DropLine dl && dl.MonsterIndex == monsterIdx) at = i; // 마지막 드랍 줄
        if (at < 0)
        {
            for (int i = 0; i < _lines.Count; i++)
                if (_lines[i] is DropsHeaderLine h && h.MonsterIndex == monsterIdx) { at = i; break; }
        }
        if (at < 0) return; // drops 블록 없음

        var line = new DropLine { Prefix = "      - ", Dirty = true, MonsterIndex = monsterIdx, Drop = drop, Flow = () => DropFlow(drop) };
        drop.PropertyChanged += (_, __) => line.Dirty = true;
        _lines.Insert(at + 1, line);
    }

    /// <summary>드랍 삭제: 그 drop 을 나타내는 줄을 제거. Attach로 되돌림.</summary>
    public void DetachDrop(DropRule drop)
    {
        for (int i = 0; i < _lines.Count; i++)
            if (_lines[i] is DropLine dl && ReferenceEquals(dl.Drop, drop)) { _lines.RemoveAt(i); return; }
    }

    public byte[] SaveBytes()
    {
        // 비활성 삽입줄(미편집 옵션 필드)은 아예 제외 → 미편집 저장 바이트 동등 유지. 활성 삽입줄만 새 줄로 등장(F9).
        var text = string.Join(_newline, _lines.Where(l => l is not InsertedLine ins || ins.Active).Select(l => l.Emit()));
        return TextFileIo.ComposeBytes(text, _bom);
    }

    // --- YamlDotNet 모델 파싱 ---
    private sealed class FileDto { public List<MonsterDto>? Monsters { get; set; } }
    private sealed class MonsterDto
    {
        public string? Name { get; set; }
        public int Type { get; set; }
        public int Hp { get; set; }
        public int AtkPower { get; set; }
        public int MoveSpeed { get; set; }
        public int AggroRange { get; set; }
        public int AttackRange { get; set; }
        public float AttackCooldown { get; set; }
        public bool IsBoss { get; set; }
        public int Cap { get; set; }
        public List<DropDto>? Drops { get; set; }
    }
    private sealed class DropDto { public int Item { get; set; } public int ChancePermil { get; set; } public int Min { get; set; } public int Max { get; set; } }

    private static List<MonsterTemplate> ParseModel(string text)
    {
        var deserializer = new DeserializerBuilder()
            .WithNamingConvention(CamelCaseNamingConvention.Instance)
            .IgnoreUnmatchedProperties()
            .Build();
        var dto = deserializer.Deserialize<FileDto>(text) ?? new FileDto();

        var list = new List<MonsterTemplate>();
        foreach (var m in dto.Monsters ?? new List<MonsterDto>())
        {
            var mt = new MonsterTemplate
            {
                Name = m.Name ?? "", Type = m.Type, Hp = m.Hp, AtkPower = m.AtkPower,
                MoveSpeed = m.MoveSpeed, AggroRange = m.AggroRange, AttackRange = m.AttackRange,
                AttackCooldown = m.AttackCooldown, IsBoss = m.IsBoss, Cap = m.Cap,
            };
            foreach (var d in m.Drops ?? new List<DropDto>())
                mt.Drops.Add(new DropRule { Item = d.Item, ChancePermil = d.ChancePermil, Min = d.Min, Max = d.Max });
            list.Add(mt);
        }
        return list;
    }
}
