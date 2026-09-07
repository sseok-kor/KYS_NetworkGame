using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DataEditor.Core;
using DataEditor.Edit;
using DataEditor.Model;
using static DataEditor.Core.EditorConstants;

namespace DataEditor.Views;

/// <summary>
/// 한 맵의 공간 데이터를 렌더·편집하는 캔버스(설계 §7·리뷰 #13).
/// - 렌더: 통행 격자(WriteableBitmap 셀해상도 50x NearestNeighbor)·격자선·스폰 사각(라벨)·포탈 원(라벨)·respawn 십자.
/// - 편집(Tool): 0=통행 페인트(좌드래그로 벽↔통행), 1=선택/이동(스폰 사각·포탈 트리거 드래그 이동).
///   모든 편집은 스트로크/드래그 1개 = undo 1항목(리뷰 #3). EditCommitted로 상위(VM)에 커밋.
/// 조작: 좌드래그=현재 도구 · 우/휠클릭 드래그=이동(pan) · 휠=줌. 좌표: 원점 좌상단·y 아래증가.
/// </summary>
public sealed class MapCanvas : FrameworkElement
{
    public static readonly DependencyProperty DatasetProperty = DependencyProperty.Register(
        nameof(Dataset), typeof(Dataset), typeof(MapCanvas),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnDataChanged));

    public static readonly DependencyProperty MapProperty = DependencyProperty.Register(
        nameof(Map), typeof(MapData), typeof(MapCanvas),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender, OnDataChanged));

    public static readonly DependencyProperty FocusedEntityProperty = DependencyProperty.Register(
        nameof(FocusedEntity), typeof(object), typeof(MapCanvas),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty ToolProperty = DependencyProperty.Register(
        nameof(Tool), typeof(int), typeof(MapCanvas), new FrameworkPropertyMetadata(0));

    public static readonly DependencyProperty FlaggedEntitiesProperty = DependencyProperty.Register(
        nameof(FlaggedEntities), typeof(HashSet<object>), typeof(MapCanvas),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));

    public Dataset? Dataset { get => (Dataset?)GetValue(DatasetProperty); set => SetValue(DatasetProperty, value); }
    public MapData? Map { get => (MapData?)GetValue(MapProperty); set => SetValue(MapProperty, value); }
    public object? FocusedEntity { get => GetValue(FocusedEntityProperty); set => SetValue(FocusedEntityProperty, value); }
    public int Tool { get => (int)GetValue(ToolProperty); set => SetValue(ToolProperty, value); } // 0=페인트, 1=선택
    public HashSet<object>? FlaggedEntities { get => (HashSet<object>?)GetValue(FlaggedEntitiesProperty); set => SetValue(FlaggedEntitiesProperty, value); }

    /// <summary>검증에서 오류/경고가 걸린 엔티티인가(캔버스 경고 오버레이용).</summary>
    private bool IsFlagged(object o) => FlaggedEntities != null && FlaggedEntities.Contains(o);

    /// <summary>편집(페인트 스트로크/드래그)이 커밋되면 발생(상위가 CommandHistory에 Push + 재검증).</summary>
    public event Action<IEditAction>? EditCommitted;

    private static readonly Typeface Face = new("Segoe UI");
    private WriteableBitmap? _walkBmp;
    private double _scale = 1;
    private Vector _offset;
    private bool _fitted;
    private bool _panning;
    private Point _lastPan;

    // 통행 페인트
    private bool _painting;
    private bool _paintTarget;
    private List<(int col, int row, bool oldVal)> _stroke = new();

    // 스폰/포탈/부활점 드래그 이동 + 스폰 사각 리사이즈
    private SpawnGroup? _dragSpawn;
    private Portal? _dragPortal;
    private MapData? _dragRespawn; // 현재 맵의 부활 좌표(RespawnX/Y) 드래그
    private Point _dragStartWorld;
    private int _sx0, _sy0, _sx1, _sy1, _stx, _sty, _rsx, _rsy;
    private int _resizeMask; // 0=이동, 아니면 잡은 변 비트(L/R/T/B)

    private const int EdgeL = 1, EdgeR = 2, EdgeT = 4, EdgeB = 8;

    /// <summary>커서가 스폰 사각의 어느 변 근처인지 비트마스크로. 0이면 내부(이동), 아니면 그 변들을 리사이즈.</summary>
    private static int EdgeMask(SpawnGroup s, Point w, double tol)
    {
        int m = 0;
        if (System.Math.Abs(w.X - s.X0) <= tol) m |= EdgeL;
        else if (System.Math.Abs(w.X - s.X1) <= tol) m |= EdgeR;
        if (System.Math.Abs(w.Y - s.Y0) <= tol) m |= EdgeT;
        else if (System.Math.Abs(w.Y - s.Y1) <= tol) m |= EdgeB;
        return m;
    }

    /// <summary>커서가 이 맵의 부활점(RespawnX/Y) 근처인가(선택/이동 대상 판정).</summary>
    private bool NearRespawn(MapData m, Point world)
    {
        double rr = 14 / _scale;
        return System.Math.Abs(world.X - m.RespawnX) <= rr && System.Math.Abs(world.Y - m.RespawnY) <= rr;
    }

    public MapCanvas()
    {
        RenderOptions.SetBitmapScalingMode(this, BitmapScalingMode.NearestNeighbor);
        RenderOptions.SetEdgeMode(this, EdgeMode.Aliased);
        ClipToBounds = true;
        Focusable = true;
    }

    private static void OnDataChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        var c = (MapCanvas)d;
        if (e.Property == DatasetProperty) c.HookDataset(e.OldValue as Dataset, e.NewValue as Dataset);
        c.RebuildWalkable();
        c._fitted = false;
        c.InvalidateVisual();
    }

    // 스폰/포탈 추가·삭제(버튼) 시 Dataset 참조는 그대로라 DP가 안 뛴다 → 컬렉션 변경을 구독해 즉시 다시 그린다.
    private void HookDataset(Dataset? oldD, Dataset? newD)
    {
        if (oldD != null) { oldD.Spawns.CollectionChanged -= OnCollectionChanged; oldD.Portals.CollectionChanged -= OnCollectionChanged; }
        if (newD != null) { newD.Spawns.CollectionChanged += OnCollectionChanged; newD.Portals.CollectionChanged += OnCollectionChanged; }
    }

    private void OnCollectionChanged(object? sender, System.Collections.Specialized.NotifyCollectionChangedEventArgs e) => InvalidateVisual();

    private WalkableGrid? CurrentGrid()
    {
        var ds = Dataset; var m = Map;
        if (ds == null || m == null || !m.HasWalkableFile) return null;
        return ds.Walkables.TryGetValue(m.WalkableFile, out var g) ? g : null;
    }

    private void RebuildWalkable()
    {
        var grid = CurrentGrid();
        if (grid == null || grid.Cols == 0 || grid.Rows == 0) { _walkBmp = null; return; }
        var bmp = new WriteableBitmap(grid.Cols, grid.Rows, 96, 96, PixelFormats.Bgra32, null);
        var pixels = new uint[grid.Cols * grid.Rows];
        for (int r = 0; r < grid.Rows; r++)
            for (int col = 0; col < grid.Cols; col++)
                pixels[r * grid.Cols + col] = CellColor(grid.GetCell(col, r));
        bmp.WritePixels(new Int32Rect(0, 0, grid.Cols, grid.Rows), pixels, grid.Cols * 4, 0);
        _walkBmp = bmp;
    }

    private static uint CellColor(bool walkable) => walkable ? 0x00000000u : 0xFF787878u;

    private void SetBmpCell(int col, int row, bool walkable)
    {
        if (_walkBmp == null) return;
        _walkBmp.WritePixels(new Int32Rect(col, row, 1, 1), new[] { CellColor(walkable) }, 4, 0);
    }

    protected override void OnRender(DrawingContext dc)
    {
        dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, ActualWidth, ActualHeight));
        var m = Map;
        if (m == null || m.Width <= 0 || m.Height <= 0) return;

        EnsureFit(m);
        dc.PushTransform(new TranslateTransform(_offset.X, _offset.Y));
        dc.PushTransform(new ScaleTransform(_scale, _scale));

        var mapRect = new Rect(0, 0, m.Width, m.Height);
        if (_walkBmp != null) dc.DrawImage(_walkBmp, mapRect);

        DrawGridLines(dc, m);
        DrawSpawns(dc, m);
        DrawPortals(dc, m);
        DrawRespawn(dc, m);
        DrawFocus(dc, m);
        dc.DrawRectangle(null, ScaledPen(Brushes.Black, 2), mapRect);

        dc.Pop();
        dc.Pop();
    }

    private Pen ScaledPen(Brush b, double px)
    {
        var p = new Pen(b, px / _scale);
        p.Freeze();
        return p;
    }

    private FormattedText Label(string text, double screenPx, Brush brush)
        => new(text, CultureInfo.CurrentCulture, FlowDirection.LeftToRight, Face,
               screenPx / _scale, brush, VisualTreeHelper.GetDpi(this).PixelsPerDip);

    private void DrawGridLines(DrawingContext dc, MapData m)
    {
        var fine = ScaledPen(new SolidColorBrush(Color.FromArgb(35, 0, 0, 0)), 1);
        var aoi = ScaledPen(new SolidColorBrush(Color.FromArgb(70, 0, 0, 130)), 1);
        for (int x = 0; x <= m.Width; x += WalkCellSize)
            dc.DrawLine(x % AoiCellSize == 0 ? aoi : fine, new Point(x, 0), new Point(x, m.Height));
        for (int y = 0; y <= m.Height; y += WalkCellSize)
            dc.DrawLine(y % AoiCellSize == 0 ? aoi : fine, new Point(0, y), new Point(m.Width, y));
    }

    private static readonly Color[] TypeColors =
    {
        Color.FromRgb(0xE7,0x4C,0x3C), Color.FromRgb(0xE6,0x7E,0x22), Color.FromRgb(0xF1,0xC4,0x0F),
        Color.FromRgb(0x2E,0xCC,0x71), Color.FromRgb(0x1A,0xBC,0x9C), Color.FromRgb(0x34,0x98,0xDB),
        Color.FromRgb(0x9B,0x59,0xB6), Color.FromRgb(0xE9,0x1E,0x63), Color.FromRgb(0x7F,0x8C,0x8D),
    };

    private static string TypeName(int type)
        => type is >= 0 and < MonsterTypeCount ? ((MonsterType)type).ToString() : $"type{type}";

    private void DrawSpawns(DrawingContext dc, MapData m)
    {
        var ds = Dataset; if (ds == null) return;
        foreach (var s in ds.Spawns)
        {
            if (s.MapId != m.MapId) continue;
            var col = TypeColors[Math.Clamp(s.Type, 0, TypeColors.Length - 1)];
            var fill = new SolidColorBrush(Color.FromArgb(55, col.R, col.G, col.B)); fill.Freeze();
            dc.DrawRectangle(fill, ScaledPen(new SolidColorBrush(col), 2),
                new Rect(s.X0, s.Y0, s.X1 - s.X0, s.Y1 - s.Y0));
            var darker = new SolidColorBrush(Color.FromRgb((byte)(col.R / 2), (byte)(col.G / 2), (byte)(col.B / 2)));
            dc.DrawText(Label($"{TypeName(s.Type)} x{s.Count}", 12, darker),
                new Point(s.X0 + 3 / _scale, s.Y0 + 2 / _scale));
            if (IsFlagged(s))
            {
                double pad = 3 / _scale;
                dc.DrawRectangle(null, WarnPen(), new Rect(s.X0 - pad, s.Y0 - pad, (s.X1 - s.X0) + 2 * pad, (s.Y1 - s.Y0) + 2 * pad));
                dc.DrawText(Label("!", 15, Brushes.OrangeRed), new Point(s.X1 - 12 / _scale, s.Y0 + 2 / _scale));
            }
        }
    }

    private Pen WarnPen()
    {
        var p = new Pen(Brushes.OrangeRed, 3 / _scale) { DashStyle = new DashStyle(new double[] { 3, 2 }, 0) };
        p.Freeze();
        return p;
    }

    private void DrawPortals(DrawingContext dc, MapData m)
    {
        var ds = Dataset; if (ds == null) return;
        var trigFill = new SolidColorBrush(Color.FromArgb(45, 0, 150, 0)); trigFill.Freeze();
        foreach (var p in ds.Portals)
        {
            if (p.SrcMapId == m.MapId)
            {
                dc.DrawEllipse(trigFill, ScaledPen(Brushes.DarkGreen, 2), new Point(p.TriggerX, p.TriggerY), p.TriggerRadius, p.TriggerRadius);
                dc.DrawText(Label($"→맵{p.DstMapId}", 12, Brushes.DarkGreen), new Point(p.TriggerX + 4 / _scale, p.TriggerY - 16 / _scale));
                if (IsFlagged(p))
                    dc.DrawEllipse(null, WarnPen(), new Point(p.TriggerX, p.TriggerY), p.TriggerRadius + 5 / _scale, p.TriggerRadius + 5 / _scale);
            }
            if (p.DstMapId == m.MapId)
                dc.DrawEllipse(Brushes.RoyalBlue, null, new Point(p.DstX, p.DstY), 8 / _scale, 8 / _scale);
        }
    }

    private void DrawRespawn(DrawingContext dc, MapData m)
    {
        double r = 12 / _scale;
        var pen = ScaledPen(Brushes.Red, 2);
        dc.DrawLine(pen, new Point(m.RespawnX - r, m.RespawnY), new Point(m.RespawnX + r, m.RespawnY));
        dc.DrawLine(pen, new Point(m.RespawnX, m.RespawnY - r), new Point(m.RespawnX, m.RespawnY + r));
        dc.DrawText(Label("부활", 12, Brushes.Red), new Point(m.RespawnX + r, m.RespawnY + 2 / _scale));
    }

    private void DrawFocus(DrawingContext dc, MapData m)
    {
        var pen = ScaledPen(Brushes.DeepPink, 3);
        if (FocusedEntity is SpawnGroup s && s.MapId == m.MapId)
        {
            double pad = 4 / _scale;
            dc.DrawRectangle(null, pen, new Rect(s.X0 - pad, s.Y0 - pad, (s.X1 - s.X0) + 2 * pad, (s.Y1 - s.Y0) + 2 * pad));
        }
        else if (FocusedEntity is Portal p)
        {
            if (p.SrcMapId == m.MapId)
                dc.DrawEllipse(null, pen, new Point(p.TriggerX, p.TriggerY), p.TriggerRadius + 6 / _scale, p.TriggerRadius + 6 / _scale);
            if (p.DstMapId == m.MapId)
                dc.DrawEllipse(null, pen, new Point(p.DstX, p.DstY), 14 / _scale, 14 / _scale);
        }
    }

    private void EnsureFit(MapData m)
    {
        if (_fitted || ActualWidth <= 0 || ActualHeight <= 0 || m.Width <= 0 || m.Height <= 0) return;
        _scale = Math.Min(ActualWidth / m.Width, ActualHeight / m.Height) * 0.95;
        _offset = new Vector((ActualWidth - m.Width * _scale) / 2, (ActualHeight - m.Height * _scale) / 2);
        _fitted = true;
    }

    private Point ToWorld(Point screen) => new((screen.X - _offset.X) / _scale, (screen.Y - _offset.Y) / _scale);

    private (int col, int row)? ScreenToCell(Point screen)
    {
        var grid = CurrentGrid(); var m = Map;
        if (grid == null || m == null) return null;
        var w = ToWorld(screen);
        if (w.X < 0 || w.X > m.Width || w.Y < 0 || w.Y > m.Height) return null;
        return (PortedGeo.CellIndexOf((int)w.X, grid.Cols), PortedGeo.CellIndexOf((int)w.Y, grid.Rows));
    }

    private void PaintCell(int col, int row)
    {
        var grid = CurrentGrid(); if (grid == null) return;
        if (grid.GetCell(col, row) == _paintTarget) return;
        _stroke.Add((col, row, grid.GetCell(col, row)));
        grid.SetCell(col, row, _paintTarget);
        SetBmpCell(col, row, _paintTarget);
        InvalidateVisual();
    }

    /// <summary>선택 모드 히트테스트: 포탈 트리거(원) 우선, 그다음 스폰 사각(내부).</summary>
    private object? HitTest(Point world)
    {
        var ds = Dataset; var m = Map;
        if (ds == null || m == null) return null;
        foreach (var p in ds.Portals)
        {
            if (p.SrcMapId != m.MapId) continue;
            double dx = world.X - p.TriggerX, dy = world.Y - p.TriggerY;
            if (dx * dx + dy * dy <= (double)p.TriggerRadius * p.TriggerRadius) return p;
        }
        foreach (var s in ds.Spawns)
        {
            if (s.MapId != m.MapId) continue;
            if (world.X >= s.X0 && world.X <= s.X1 && world.Y >= s.Y0 && world.Y <= s.Y1) return s;
        }
        return null;
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        var pos = e.GetPosition(this);
        double wx = (pos.X - _offset.X) / _scale, wy = (pos.Y - _offset.Y) / _scale;
        _scale *= e.Delta > 0 ? 1.1 : 1 / 1.1;
        _scale = Math.Clamp(_scale, 0.02, 20);
        _offset = new Vector(pos.X - wx * _scale, pos.Y - wy * _scale);
        InvalidateVisual();
    }

    protected override void OnMouseDown(MouseButtonEventArgs e)
    {
        if (e.ChangedButton == MouseButton.Left)
        {
            if (Tool == 0) // 통행 페인트
            {
                var grid = CurrentGrid();
                if (grid != null && ScreenToCell(e.GetPosition(this)) is { } c)
                {
                    _painting = true;
                    _paintTarget = !grid.GetCell(c.col, c.row);
                    _stroke = new List<(int, int, bool)>();
                    Focus(); CaptureMouse();
                    PaintCell(c.col, c.row);
                }
            }
            else // 선택/이동
            {
                var world = ToWorld(e.GetPosition(this));
                var m = Map;
                if (m != null && NearRespawn(m, world)) // 부활점(작은 타겟)을 스폰 사각보다 먼저
                {
                    _dragRespawn = m; _dragStartWorld = world; _rsx = m.RespawnX; _rsy = m.RespawnY;
                    CaptureMouse();
                }
                else if (HitTest(world) is SpawnGroup s)
                {
                    _dragSpawn = s; _dragStartWorld = world;
                    _sx0 = s.X0; _sy0 = s.Y0; _sx1 = s.X1; _sy1 = s.Y1;
                    _resizeMask = EdgeMask(s, world, 8 / _scale); // 변 근처면 리사이즈, 내부면 이동
                    CaptureMouse();
                }
                else if (HitTest(world) is Portal p)
                {
                    _dragPortal = p; _dragStartWorld = world;
                    _stx = p.TriggerX; _sty = p.TriggerY;
                    CaptureMouse();
                }
            }
        }
        else if (e.ChangedButton is MouseButton.Middle or MouseButton.Right
                 && !_painting && _dragSpawn == null && _dragPortal == null && _dragRespawn == null) // 좌측 편집 중엔 pan 시작 금지(F12)
        {
            _panning = true; _lastPan = e.GetPosition(this); CaptureMouse();
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (_painting)
        {
            if (ScreenToCell(e.GetPosition(this)) is { } c) PaintCell(c.col, c.row);
            return;
        }
        if (_dragSpawn != null || _dragPortal != null || _dragRespawn != null)
        {
            var world = ToWorld(e.GetPosition(this));
            int dx = (int)(world.X - _dragStartWorld.X), dy = (int)(world.Y - _dragStartWorld.Y);
            if (_dragRespawn is { } rm) { rm.RespawnX = _rsx + dx; rm.RespawnY = _rsy + dy; }
            else if (_dragSpawn is { } s)
            {
                if (_resizeMask != 0)
                {
                    // 잡은 변을 커서 위치로 — 반대 변을 넘지 않게 클램프(x0<=x1, y0<=y1 유지)
                    int wx = (int)world.X, wy = (int)world.Y;
                    if ((_resizeMask & EdgeL) != 0) s.X0 = System.Math.Min(wx, s.X1);
                    if ((_resizeMask & EdgeR) != 0) s.X1 = System.Math.Max(wx, s.X0);
                    if ((_resizeMask & EdgeT) != 0) s.Y0 = System.Math.Min(wy, s.Y1);
                    if ((_resizeMask & EdgeB) != 0) s.Y1 = System.Math.Max(wy, s.Y0);
                }
                else { s.X0 = _sx0 + dx; s.Y0 = _sy0 + dy; s.X1 = _sx1 + dx; s.Y1 = _sy1 + dy; }
            }
            else if (_dragPortal is { } p) { p.TriggerX = _stx + dx; p.TriggerY = _sty + dy; }
            InvalidateVisual();
            return;
        }
        if (_panning)
        {
            var p = e.GetPosition(this);
            _offset += p - _lastPan;
            _lastPan = p;
            InvalidateVisual();
        }
        else if (Tool == 1) // 선택 모드 hover: 변 근처=리사이즈 커서, 내부=이동 커서(핸들 발견성)
        {
            UpdateHoverCursor(ToWorld(e.GetPosition(this)));
        }
    }

    private void UpdateHoverCursor(Point world)
    {
        var cur = Cursors.Arrow;
        if (Map is { } rm && NearRespawn(rm, world)) { if (Cursor != Cursors.SizeAll) Cursor = Cursors.SizeAll; return; }
        var hit = HitTest(world);
        if (hit is SpawnGroup s)
        {
            int m = EdgeMask(s, world, 8 / _scale);
            cur = m switch
            {
                EdgeL | EdgeT => Cursors.SizeNWSE,
                EdgeR | EdgeB => Cursors.SizeNWSE,
                EdgeR | EdgeT => Cursors.SizeNESW,
                EdgeL | EdgeB => Cursors.SizeNESW,
                EdgeL or EdgeR => Cursors.SizeWE,
                EdgeT or EdgeB => Cursors.SizeNS,
                _ => Cursors.SizeAll, // 내부 = 이동
            };
        }
        else if (hit is Portal) cur = Cursors.SizeAll;
        if (Cursor != cur) Cursor = cur;
    }

    protected override void OnMouseUp(MouseButtonEventArgs e)
    {
        if (_painting && e.ChangedButton == MouseButton.Left) CommitPaint();
        else if ((_dragSpawn != null || _dragPortal != null || _dragRespawn != null) && e.ChangedButton == MouseButton.Left) CommitDrag();
        else if (_panning && e.ChangedButton is MouseButton.Middle or MouseButton.Right)
        {
            _panning = false; ReleaseMouseCapture();
        }
    }

    /// <summary>어떤 이유로든 마우스 캡처를 잃으면(alt-tab, 다른 컨트롤 탈취 등) 진행 중 편집을 확정한다 —
    /// 플래그가 고착돼 hover 만으로 계속 드래그되거나 편집이 undo에 안 잡히는 것을 막는 백스톱(F12).</summary>
    protected override void OnLostMouseCapture(MouseEventArgs e)
    {
        if (_painting) CommitPaint();
        else if (_dragSpawn != null || _dragPortal != null || _dragRespawn != null) CommitDrag();
    }

    private void CommitPaint()
    {
        _painting = false;
        ReleaseMouseCapture();
        if (_stroke.Count > 0 && CurrentGrid() is { } grid)
            EditCommitted?.Invoke(new WalkableStrokeAction(this, grid, _stroke));
        _stroke = new List<(int, int, bool)>();
    }

    private void CommitDrag()
    {
        // 절대 스냅샷(시작->현재)으로 커밋 — 리사이즈는 잡은 변이 반대 변에 클램프돼 반영량이 커서 이동량과 다르므로, 델타로는 원래 좌표를 되돌릴 수 없다.
        if (_dragSpawn is { } s)
        {
            var before = (_sx0, _sy0, _sx1, _sy1);
            var after = (s.X0, s.Y0, s.X1, s.Y1);
            if (before != after) EditCommitted?.Invoke(new MoveSpawnAction(this, s, before, after));
        }
        else if (_dragPortal is { } p)
        {
            var before = (_stx, _sty);
            var after = (p.TriggerX, p.TriggerY);
            if (before != after) EditCommitted?.Invoke(new MovePortalAction(this, p, before, after));
        }
        else if (_dragRespawn is { } rm)
        {
            var before = (_rsx, _rsy);
            var after = (rm.RespawnX, rm.RespawnY);
            if (before != after) EditCommitted?.Invoke(new MoveRespawnAction(this, rm, before, after));
        }
        _dragSpawn = null; _dragPortal = null; _dragRespawn = null; _resizeMask = 0;
        ReleaseMouseCapture();
    }

    // ---------------- undo 액션 ----------------
    private sealed class WalkableStrokeAction : IEditAction
    {
        private readonly MapCanvas _canvas;
        private readonly WalkableGrid _grid;
        private readonly List<(int col, int row, bool oldVal)> _changes;

        public WalkableStrokeAction(MapCanvas canvas, WalkableGrid grid, List<(int, int, bool)> changes)
        { _canvas = canvas; _grid = grid; _changes = changes; }

        public void Execute() => Apply(useNew: true);
        public void Unexecute() => Apply(useNew: false);

        private void Apply(bool useNew)
        {
            // 이 스트로크의 맵이 지금 표시 중일 때만 미리보기 비트맵을 건드린다 —
            //   다른(더 작은) 맵이 표시 중이면 좌표가 범위 밖이라 크래시하거나 엉뚱한 맵을 오염시킨다(F13).
            //   모델 격자는 항상 갱신하므로, 그 맵을 다시 열면 RebuildWalkable이 올바른 비트맵을 재생성한다.
            bool current = _canvas.CurrentGrid() == _grid;
            foreach (var (col, row, oldVal) in _changes)
            {
                bool v = useNew ? !oldVal : oldVal;
                _grid.SetCell(col, row, v);
                if (current) _canvas.SetBmpCell(col, row, v);
            }
            if (current) _canvas.InvalidateVisual();
        }
    }

    // 이동 undo는 델타가 아니라 시작/끝 절대 좌표를 저장한다 — 리사이즈는 잡은 변이 반대 변에 클램프되므로
    //   반영량이 커서 이동량과 달라진다. Unexecute/Execute가 언제나 "실제 존재했던" 좌표를 그대로 세팅해,
    //   델타 누적이 만드는 x0>x1 같은 비존재 상태를 배제한다.
    private sealed class MoveSpawnAction : IEditAction
    {
        private readonly MapCanvas _c; private readonly SpawnGroup _s;
        private readonly (int x0, int y0, int x1, int y1) _before, _after;
        public MoveSpawnAction(MapCanvas c, SpawnGroup s, (int x0, int y0, int x1, int y1) before, (int x0, int y0, int x1, int y1) after)
        { _c = c; _s = s; _before = before; _after = after; }
        public void Execute() => Set(_after);
        public void Unexecute() => Set(_before);
        private void Set((int x0, int y0, int x1, int y1) v)
        {
            _s.X0 = v.x0; _s.Y0 = v.y0; _s.X1 = v.x1; _s.Y1 = v.y1;
            _c.InvalidateVisual();
        }
    }

    private sealed class MovePortalAction : IEditAction
    {
        private readonly MapCanvas _c; private readonly Portal _p;
        private readonly (int x, int y) _before, _after;
        public MovePortalAction(MapCanvas c, Portal p, (int x, int y) before, (int x, int y) after)
        { _c = c; _p = p; _before = before; _after = after; }
        public void Execute() => Set(_after);
        public void Unexecute() => Set(_before);
        private void Set((int x, int y) v)
        {
            _p.TriggerX = v.x; _p.TriggerY = v.y;
            _c.InvalidateVisual();
        }
    }

    private sealed class MoveRespawnAction : IEditAction
    {
        private readonly MapCanvas _c; private readonly MapData _m;
        private readonly (int x, int y) _before, _after;
        public MoveRespawnAction(MapCanvas c, MapData m, (int x, int y) before, (int x, int y) after)
        { _c = c; _m = m; _before = before; _after = after; }
        public void Execute() => Set(_after);
        public void Unexecute() => Set(_before);
        private void Set((int x, int y) v)
        {
            _m.RespawnX = v.x; _m.RespawnY = v.y; // MapData 프로퍼티 변경 -> maps 테이블 dirty + 재검증 자동
            _c.InvalidateVisual();
        }
    }
}
