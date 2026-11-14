# Builds the shipped pipeline list config/predator/pipelines.fcpl from collected pipeline records (reproducible:
# the same inputs give the same file, byte for byte).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pipelines_build.ps1 -In out\pipelines\collect\<run>,...
#       [-Base config\predator\pipelines.fcpl] [-Out config\predator\pipelines.fcpl]
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pipelines_build.ps1 -Info <file.fcpl>
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pipelines_build.ps1 -Coverage <file.fcpl> -In <collect dir>
# Inputs: folders written by runs with FCP_PIPELINE_COLLECT=<folder> (tools\pipelines_collect.ps1). Every file
# <group>.bin there holds the pipeline records the renderer saw while that group was active ("menu" = before the
# first level, otherwise the level name). -Base merges an existing list (its records and groups are kept).
# -Coverage: how many records of the collected runs are not in the list, per group (a cold first visit with the
# list then creates about that many pipelines).
#
# What a record holds (and nothing else): the hashes of the two guest shader containers (vertex and pixel shader,
# the key of the translated shaders, not the shaders), the primitive type, the host formats of the bound targets
# and the render state registers that make up a pipeline (blend control, write masks, depth/stencil control,
# stencil masks without the reference value, cull mode/face, depth clip, depth bias), plus the guest MSAA level and
# a "screen-sized target" bit so the loader derives the host sample count for the player's MSAA setting. No shader
# code, no constants, no textures or other game data. The file is the same for Vulkan and D3D12.
#
# File format (little-endian), version 1:
#   header 40 bytes: u32 magic "FCPL", u32 version 1, u32 header size 40, u32 record size 112, u32 record count,
#                    u32 group count (<= 32), u32 group name size 32, u32 flags 0, u64 FNV-1a 64 of everything after
#                    the header
#   group names: group count x 32 bytes, ASCII, zero padded (bit i of a record's mask = group i)
#   records: 104 bytes as in the renderer's pipeline list (PipeRec, src/renderer/core/game_renderer.cpp) with the
#            sample count field 0 and the variant word (0x5A0000xx), then u32 group mask, u32 0
param(
  [string[]]$In = @(),
  [string]$Base = "",
  [string]$Out = "",
  [string]$Info = "",
  [string]$Coverage = ""
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Out) { $Out = Join-Path $root "config\predator\pipelines.fcpl" }
if ($In.Count -eq 1 -and $In[0].Contains(",")) { $In = $In[0].Split(",") }
function Abs([string]$p) { if ([IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $root $p } }

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

public static class FcpPipelineList {
  public const uint LocalMagic = 0x31504346;   // "FCP1": renderer pipeline list / collected records
  public const uint ShippedMagic = 0x4C504346; // "FCPL"
  public const int PipeRecSize = 104, RecordSize = 112, HeaderSize = 40, NameSize = 32, MaxGroups = 32;
  public const int PadOffset = 96, VariantOffset = 100;
  public const uint VariantMark = 0x5A000000, VariantMarkMask = 0xFF000000;
  // Campaign order (menu first): the prewarm takes the groups in this order unless a level is being loaded.
  public static readonly string[] Order = {
    "menu", "00_training", "01_beach", "02_native", "03_river", "04_rain_forest", "05_mines", "06_cliffs",
    "07_sub-aquatic_facility", "10_dead_marshes", "11_wasteland", "13_black_forest", "21_dunea", "22_duneb",
    "23_dunec", "24_shantyday", "26_shantynight", "28_jungle", "29_forest", "30_mountain" };

  public static ulong Fnv(byte[] b, int from) {
    ulong h = 0xcbf29ce484222325UL;
    for (int i = from; i < b.Length; ++i) { h ^= b[i]; h *= 0x100000001b3UL; }
    return h;
  }
  static string Hex(byte[] r) { return BitConverter.ToString(r).Replace("-", ""); }

  public class Rec { public byte[] Bytes; public uint Groups; }
  public class List { public List<string> Groups = new List<string>(); public Dictionary<string, Rec> Recs = new Dictionary<string, Rec>(); public int Skipped; }

  // Normalised 104 bytes: sample count 0 (derived when loading); null without a valid variant word.
  public static byte[] Normalise(byte[] r) {
    uint variant = BitConverter.ToUInt32(r, VariantOffset);
    if ((variant & VariantMarkMask) != VariantMark || (variant & 0x00FFFFF8) != 0) return null;
    byte[] n = (byte[])r.Clone();
    Array.Clear(n, PadOffset, 4);
    return n;
  }

  public static int GroupIndex(List l, string name) {
    int i = l.Groups.IndexOf(name);
    if (i >= 0) return i;
    if (l.Groups.Count >= MaxGroups) throw new Exception("more than " + MaxGroups + " groups (" + name + ")");
    l.Groups.Add(name);
    return l.Groups.Count - 1;
  }

  public static void Add(List l, byte[] r, uint groups) {
    byte[] n = Normalise(r);
    if (n == null) { l.Skipped++; return; }
    string k = Hex(n);
    Rec e;
    if (l.Recs.TryGetValue(k, out e)) e.Groups |= groups;
    else l.Recs[k] = new Rec { Bytes = n, Groups = groups };
  }

  // A collected file (FCP1 header, 104-byte records) as group `group`.
  public static int ReadCollected(List l, string path, string group) {
    byte[] b = File.ReadAllBytes(path);
    if (b.Length < 8 || BitConverter.ToUInt32(b, 0) != LocalMagic || BitConverter.ToUInt32(b, 4) != PipeRecSize)
      throw new Exception(path + ": not a pipeline record file (FCP1/104)");
    uint bit = 1u << GroupIndex(l, group);
    int n = 0;
    for (int at = 8; at + PipeRecSize <= b.Length; at += PipeRecSize, ++n) {
      byte[] r = new byte[PipeRecSize];
      Buffer.BlockCopy(b, at, r, 0, PipeRecSize);
      Add(l, r, bit);
    }
    return n;
  }

  public static List ReadShipped(string path) {
    byte[] b = File.ReadAllBytes(path);
    if (b.Length < HeaderSize) throw new Exception(path + ": too short");
    uint magic = BitConverter.ToUInt32(b, 0), version = BitConverter.ToUInt32(b, 4), hsize = BitConverter.ToUInt32(b, 8);
    uint rsize = BitConverter.ToUInt32(b, 12), count = BitConverter.ToUInt32(b, 16), gcount = BitConverter.ToUInt32(b, 20);
    uint nsize = BitConverter.ToUInt32(b, 24);
    ulong sum = BitConverter.ToUInt64(b, 32);
    if (magic != ShippedMagic || version != 1 || hsize != HeaderSize || rsize != RecordSize || nsize != NameSize || gcount > MaxGroups)
      throw new Exception(path + ": not a pipeline list version 1");
    if (b.Length != HeaderSize + gcount * NameSize + (long)count * RecordSize) throw new Exception(path + ": wrong length");
    if (Fnv(b, HeaderSize) != sum) throw new Exception(path + ": checksum mismatch");
    List l = new List();
    for (int g = 0; g < gcount; ++g) l.Groups.Add(Encoding.ASCII.GetString(b, HeaderSize + g * NameSize, NameSize).TrimEnd('\0'));
    int at = HeaderSize + (int)gcount * NameSize;
    for (int i = 0; i < count; ++i, at += RecordSize) {
      byte[] r = new byte[PipeRecSize];
      Buffer.BlockCopy(b, at, r, 0, PipeRecSize);
      uint groups = BitConverter.ToUInt32(b, at + PipeRecSize);
      if (Normalise(r) == null || BitConverter.ToUInt32(r, PadOffset) != 0) throw new Exception(path + ": record " + i + " invalid");
      Add(l, r, groups);
    }
    return l;
  }

  static int RankOf(string g) { int i = Array.IndexOf(Order, g); return i >= 0 ? i : Order.Length; }

  // Deterministic: groups in campaign order (others by name), records by their lowest group, then by content.
  public static byte[] Write(List l) {
    List<string> groups = l.Groups.OrderBy(g => RankOf(g)).ThenBy(g => g, StringComparer.Ordinal).ToList();
    int[] remap = l.Groups.Select(g => groups.IndexOf(g)).ToArray();
    var recs = l.Recs.Values.Select(r => {
      uint m = 0;
      for (int i = 0; i < remap.Length; ++i) if ((r.Groups & (1u << i)) != 0) m |= 1u << remap[i];
      return new Rec { Bytes = r.Bytes, Groups = m };
    }).ToList();
    Func<uint, int> low = m => { for (int i = 0; i < 32; ++i) if ((m & (1u << i)) != 0) return i; return 32; };
    recs.Sort((a, b) => {
      int c = low(a.Groups).CompareTo(low(b.Groups));
      if (c != 0) return c;
      return string.CompareOrdinal(Hex(a.Bytes), Hex(b.Bytes));
    });
    using (var ms = new MemoryStream()) {
      var w = new BinaryWriter(ms);
      w.Write(ShippedMagic); w.Write(1u); w.Write((uint)HeaderSize); w.Write((uint)RecordSize); w.Write((uint)recs.Count);
      w.Write((uint)groups.Count); w.Write((uint)NameSize); w.Write(0u); w.Write(0UL);
      foreach (string g in groups) {
        byte[] n = new byte[NameSize];
        byte[] a = Encoding.ASCII.GetBytes(g);
        if (a.Length >= NameSize) throw new Exception("group name too long: " + g);
        Buffer.BlockCopy(a, 0, n, 0, a.Length);
        w.Write(n);
      }
      foreach (Rec r in recs) { w.Write(r.Bytes); w.Write(r.Groups); w.Write(0u); }
      w.Flush();
      byte[] b = ms.ToArray();
      byte[] s = BitConverter.GetBytes(Fnv(b, HeaderSize));
      Buffer.BlockCopy(s, 0, b, 32, 8);
      return b;
    }
  }

  public static string Describe(List l) {
    var sb = new StringBuilder();
    sb.AppendFormat("{0} records, {1} groups\n", l.Recs.Count, l.Groups.Count);
    for (int g = 0; g < l.Groups.Count; ++g) {
      uint bit = 1u << g;
      int all = l.Recs.Values.Count(r => (r.Groups & bit) != 0), only = l.Recs.Values.Count(r => r.Groups == bit);
      sb.AppendFormat("  {0,-26} {1,6} records, {2,5} only there\n", l.Groups[g], all, only);
    }
    return sb.ToString();
  }

  // Records of `seen` (per group) that `list` lacks: what a cold first visit with the list still creates.
  public static string Missing(List list, List seen) {
    var sb = new StringBuilder();
    for (int g = 0; g < seen.Groups.Count; ++g) {
      uint bit = 1u << g;
      var mine = seen.Recs.Where(kv => (kv.Value.Groups & bit) != 0).ToList();
      int miss = mine.Count(kv => !list.Recs.ContainsKey(kv.Key));
      sb.AppendFormat("  {0,-26} {1,6} seen, {2,5} not in the list\n", seen.Groups[g], mine.Count, miss);
    }
    return sb.ToString();
  }
}
'@

function ReadDirs($list, [string[]]$dirs) {
  foreach ($d in $dirs) {
    $p = Abs $d
    $files = if (Test-Path -LiteralPath $p -PathType Container) { Get-ChildItem -LiteralPath $p -File -Filter *.bin } else { @(Get-Item -LiteralPath $p) }
    foreach ($f in $files | Sort-Object FullName) {
      $n = [FcpPipelineList]::ReadCollected($list, $f.FullName, $f.BaseName.ToLowerInvariant())
      $shown = if ($f.FullName.StartsWith($root)) { $f.FullName.Substring($root.Length + 1) } else { $f.FullName }
      Write-Output ("  {0}: {1} records" -f $shown, $n)
    }
  }
}

if ($Info) {
  $l = [FcpPipelineList]::ReadShipped((Abs $Info))
  Write-Output ("{0}: {1:N0} bytes, checksum ok" -f $Info, (Get-Item (Abs $Info)).Length)
  Write-Output ([FcpPipelineList]::Describe($l))
  exit 0
}
if ($Coverage) {
  $l = [FcpPipelineList]::ReadShipped((Abs $Coverage))
  $seen = New-Object FcpPipelineList+List
  ReadDirs $seen $In
  Write-Output "coverage of $Coverage against the collected runs:"
  Write-Output ([FcpPipelineList]::Missing($l, $seen))
  exit 0
}
if ($In.Count -eq 0 -and -not $Base) { throw "-In <collect folders> (or -Base) needed" }
$list = if ($Base) { [FcpPipelineList]::ReadShipped((Abs $Base)) } else { New-Object FcpPipelineList+List }
ReadDirs $list $In
if ($list.Skipped) { Write-Output "pipelines_build: $($list.Skipped) records without variant word skipped (older renderer)" }
$bytes = [FcpPipelineList]::Write($list)
[IO.File]::WriteAllBytes((Abs $Out), $bytes)
$check = [FcpPipelineList]::ReadShipped((Abs $Out))
Write-Output ("pipelines_build: {0} ({1:N0} bytes), sha256 {2}" -f $Out, $bytes.Length, (Get-FileHash -Algorithm SHA256 (Abs $Out)).Hash.ToLowerInvariant())
Write-Output ([FcpPipelineList]::Describe($check))
