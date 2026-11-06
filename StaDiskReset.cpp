// ============================================================================
//  StaDiskReset  —  磁盘已删除文件恢复 / 内容特征扫描（单文件命令行工具）
//  ---------------------------------------------------------------------------
//  功能：
//    1. 文件名恢复：解析 NTFS $MFT，列出已删除记录，按名字恢复内容
//    2. 文件内容恢复：扫描空闲簇，按特征串 / JSON 结构找回内容
//    3. 实时进度条、中文界面、UTF-8 输出
//  只读工具：对源盘只做读取，绝不写入
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <cctype>
#include <io.h>
#include <conio.h>

using std::string;
using std::cout;
using std::cin;
using std::endl;
using std::vector;
using std::wstring;

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t  i64;
typedef int8_t   i8;

// ---------------------------------------------------------------------------
// 编码 / 输出
// ---------------------------------------------------------------------------
static string W2U(const wstring& w) {
    if (w.empty()) return string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static wstring U2W(const string& s) {
    if (s.empty()) return wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static void out(const char* s) { fputs(s, stdout); }

static string fmtSize(u64 n) {
    char b[64];
    if (n >= (1ull << 30))      snprintf(b, sizeof b, "%.2f GB", (double)n / (double)(1ull << 30));
    else if (n >= (1ull << 20)) snprintf(b, sizeof b, "%.2f MB", (double)n / (double)(1ull << 20));
    else if (n >= (1ull << 10)) snprintf(b, sizeof b, "%.1f KB", (double)n / (double)(1ull << 10));
    else                        snprintf(b, sizeof b, "%llu B", (unsigned long long)n);
    return b;
}
static string fmtTime(u64 ft) {
    if (ft == 0) return "-";
    FILETIME f; f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF); f.dwHighDateTime = (DWORD)(ft >> 32);
    FILETIME loc;
    if (!FileTimeToLocalFileTime(&f, &loc)) return "-";
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&loc, &st)) return "-";
    char b[64];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

// ---------------------------------------------------------------------------
// 进度条
// ---------------------------------------------------------------------------
static bool g_tty = false;
static DWORD g_ioErr = 0;
static u64 g_parsed = 0, g_failed = 0;
static int  g_lastPct = -1;

static void progress(const char* label, u64 done, u64 total, double secs, u64 hits) {
    if (total == 0) total = 1;
    int pct = (int)((double)done * 100.0 / (double)total);
    if (pct > 100) pct = 100;
    if (!g_tty) {
        if (pct / 5 == g_lastPct / 5) return;
        g_lastPct = pct;
        printf("[%s] %3d%%  %s / %s  hits=%llu\n", label, pct,
               fmtSize(done).c_str(), fmtSize(total).c_str(), (unsigned long long)hits);
        fflush(stdout);
        return;
    }
    const int W = 28;
    int fill = pct * W / 100;
    char bar[64];
    for (int i = 0; i < W; i++) bar[i] = (i < fill) ? '#' : '-';
    bar[W] = 0;
    double mbs = secs > 0.001 ? (double)done / (1024.0 * 1024.0) / secs : 0.0;
    double eta = mbs > 0.001 ? ((double)(total - done) / (1024.0 * 1024.0)) / mbs : 0.0;
    int em = (int)(eta / 60), es = (int)eta % 60;
    printf("\r[%s] %3d%%  %8.1f MB/s  ETA %02d:%02d  hits=%llu   ",
           bar, pct, mbs, em, es, (unsigned long long)hits);
    fflush(stdout);
    g_lastPct = pct;
}
static void progressDone() { if (g_tty) { printf("\n"); fflush(stdout); } }

// ---------------------------------------------------------------------------
// 卷
// ---------------------------------------------------------------------------
struct Run { u64 vcn; i64 lcn; u64 count; };

struct Volume {
    wchar_t letter = 0;
    HANDLE  h = INVALID_HANDLE_VALUE;
    u32     bps = 512, spc = 8;
    u64     clusterSize = 4096;
    u64     totalClusters = 0;
    u64     mftLcn = 0;
    u32     mftRecordSize = 1024;

    ~Volume() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }

    bool open(wchar_t L, string& err) {
        letter = L;
        wchar_t path[16];
        swprintf(path, 16, L"\\\\.\\%c:", L);
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED) err = "无法打开卷（拒绝访问）—— 请用【管理员身份】运行本程序";
            else { char b[128]; snprintf(b, sizeof b, "无法打开卷，错误码 %lu", (unsigned long)e); err = b; }
            return false;
        }
        u8 bs[512];
        if (!readAligned(0, bs, 512)) { err = "读取引导扇区失败"; return false; }
        if (memcmp(bs + 3, "NTFS    ", 8) != 0) {
            err = "该卷不是 NTFS 格式，本工具目前只支持 NTFS";
            return false;
        }
        bps = *(u16*)(bs + 0x0B);
        spc = *(u8*)(bs + 0x0D);
        if (bps == 0 || spc == 0) { err = "引导扇区参数异常"; return false; }
        clusterSize = (u64)bps * spc;
        u64 totalSectors = *(u64*)(bs + 0x28);
        mftLcn = *(u64*)(bs + 0x30);
        totalClusters = totalSectors / spc;
        i8 cpr = (i8)bs[0x40];
        mftRecordSize = (cpr > 0) ? (u32)cpr * (u32)clusterSize : (1u << (u32)(-cpr));
        if (mftRecordSize < 512 || mftRecordSize > 65536) { err = "MFT 记录大小异常"; return false; }
        return true;
    }

    bool readAligned(u64 off, void* buf, u32 len) {
        if (len == 0) return true;
        u8* p = (u8*)buf;
        u32 got = 0;
        while (got < len) {
            // 定位读：省掉每次 SetFilePointerEx 的系统调用（空闲区碎片多时很关键）
            OVERLAPPED ov;
            memset(&ov, 0, sizeof(ov));
            ov.Offset = (DWORD)((off + got) & 0xFFFFFFFFull);
            ov.OffsetHigh = (DWORD)((off + got) >> 32);
            DWORD rd = 0;
            if (!ReadFile(h, p + got, len - got, &rd, &ov)) { g_ioErr = GetLastError(); return false; }
            if (rd == 0) return false;
            got += rd;
        }
        return true;
    }

    // 任意偏移/长度读取：内部自动做扇区对齐
    bool readBytes(u64 off, void* buf, u32 len) {
        if (len == 0) return true;
        u64 aoff = off & ~511ull;
        u32 pre = (u32)(off - aoff);
        if (pre == 0 && (len & 511u) == 0) return readAligned(off, buf, len);
        if (pre > 0xFFFFFFFFu - len - 511u) return false;   // 防 u32 加法溢出
        u32 total = (pre + len + 511u) & ~511u;
        u8* tmp = (u8*)malloc(total);
        if (!tmp) return false;
        bool ok = readAligned(aoff, tmp, total);
        if (ok) memcpy(buf, tmp + pre, len);
        free(tmp);
        return ok;
    }
    // 按 run 表读取一个数据流的某段
    bool readStream(const vector<Run>& runs, u64 off, void* buf, u32 len, u64 streamSize) {
        if (len == 0) return true;
        if (off >= streamSize) return false;
        if (off + len > streamSize) len = (u32)(streamSize - off);
        u8* dst = (u8*)buf;
        u32 done = 0;
        u64 vcn = off / clusterSize;
        u32 offInCl = (u32)(off % clusterSize);
        while (done < len) {
            auto it = std::upper_bound(runs.begin(), runs.end(), vcn,
                                       [](u64 v, const Run& rr) { return v < rr.vcn; });
            if (it == runs.begin()) return false;
            --it;
            if (vcn >= it->vcn + it->count) return false;
            const Run* r = &(*it);
            u64 clIdx = vcn - r->vcn;
            u64 phys = (u64)(r->lcn + (i64)clIdx) * clusterSize + offInCl;
            u64 room = (r->count - clIdx) * clusterSize - offInCl;
            u32 chunk = (u32)std::min<u64>(room, (u64)(len - done));
            if (!readBytes(phys, dst + done, chunk)) return false;
            done += chunk;
            vcn = vcn + (offInCl + chunk) / clusterSize;
            offInCl = (offInCl + chunk) % clusterSize;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// 属性 / run 解析
// ---------------------------------------------------------------------------
static bool parseRunList(const u8* p, u32 len, vector<Run>& runs, u64 startVcn) {
    u64 vcn = startVcn;
    i64 prevLcn = 0;   // NTFS runlist 的偏移是相对上一个 run 的增量
    u32 i = 0;
    while (i < len) {
        u8 hdr = p[i];
        if (hdr == 0) break;
        u32 lenBytes = hdr & 0x0F;
        u32 offBytes = (hdr >> 4) & 0x0F;
        if (lenBytes == 0 || i + 1 + lenBytes + offBytes > len) return false;
        i++;
        u64 clen = 0;
        for (u32 b = 0; b < lenBytes; b++) clen |= (u64)p[i + b] << (8 * b);
        i += lenBytes;
        i64 coff = 0;
        for (u32 b = 0; b < offBytes; b++) coff |= (i64)((u64)p[i + b] << (8 * b));
        // 符号扩展
        if (offBytes > 0 && offBytes < 8) {
            u64 signBit = 1ull << (offBytes * 8 - 1);
            if ((u64)coff & signBit) coff |= (i64)(~0ull << (offBytes * 8));
        }
        i += offBytes;
        prevLcn += coff;                                   // 累加得到绝对 LCN
        runs.push_back(Run{ vcn, prevLcn, clen });
        vcn += clen;
    }
    return true;
}

// ---------------------------------------------------------------------------
// MFT 条目
// ---------------------------------------------------------------------------
struct MftEntry {
    u64 recno = 0;
    u64 parent = 0;
    u64 dataSize = 0;
    u64 allocSize = 0;
    u64 mtime = 0, ctime = 0, mftTime = 0;
    bool inUse = false, isDir = false, hasData = false;
    u8   ns = 0;
    string name;
    vector<Run> runs;
    vector<u8> resident;      // 常驻属性数据（小文件就在 MFT 记录里）
};

static bool applyFixup(u8* rec, u32 recSize, u32 sectorSize) {
    u16 usaOff = *(u16*)(rec + 4);
    u16 usaCnt = *(u16*)(rec + 6);
    if (usaCnt == 0 || usaOff + usaCnt * 2 > recSize) return false;
    u16 usn = *(u16*)(rec + usaOff);
    for (u16 i = 1; i < usaCnt; i++) {
        u32 pos = i * sectorSize - 2;
        if (pos + 2 > recSize) return false;
        if (*(u16*)(rec + pos) != usn) return false;
        *(u16*)(rec + pos) = *(u16*)(rec + usaOff + i * 2);
    }
    return true;
}

static bool parseRecord(u8* rec, u32 recSize, u32 sectorSize, u64 recno, MftEntry& e) {
    if (memcmp(rec, "FILE", 4) != 0) return false;
    if (!applyFixup(rec, recSize, sectorSize)) return false;
    e.recno = recno;
    u16 attrOff = *(u16*)(rec + 0x14);
    u16 flags = *(u16*)(rec + 0x16);
    e.inUse = (flags & 0x01) != 0;
    e.isDir = (flags & 0x02) != 0;
    u32 usedSize = *(u32*)(rec + 0x18);
    if (usedSize == 0 || usedSize > recSize) usedSize = recSize;

    bool haveName = false;
    u8 bestNs = 255;
    u32 off = attrOff;
    while (off + 4 <= usedSize) {
        u32 type = *(u32*)(rec + off);
        if (type == 0xFFFFFFFF) break;
        u32 alen = *(u32*)(rec + off + 4);
        if (alen < 8 || off + alen > usedSize) break;
        u8 nonRes = rec[off + 8];
        if (type == 0x10 && !nonRes) {
            u16 vlen = *(u16*)(rec + off + 0x10);
            u16 voff = *(u16*)(rec + off + 0x14);
            const u8* v = rec + off + voff;
            if (voff + vlen <= alen) {
                if (vlen >= 24) {          // 至少要有到 mftTime 末尾的 24 字节
                    e.ctime = *(u64*)(v + 0);
                    e.mtime = *(u64*)(v + 8);
                    e.mftTime = *(u64*)(v + 16);
                }
            }
        } else if (type == 0x30 && !nonRes) {
            u16 vlen = *(u16*)(rec + off + 0x10);
            u16 voff = *(u16*)(rec + off + 0x14);
            const u8* v = rec + off + voff;
            if (voff + vlen <= alen && vlen >= 66) {
                u64 pref = *(u64*)(v + 0) & 0xFFFFFFFFFFFFull;
                u64 mtime = *(u64*)(v + 16);
                u8 nlen = v[64];
                u8 ns = v[65];
                if (66u + (u32)nlen * 2u <= vlen) {
                    wstring name((const wchar_t*)(v + 66), nlen);
                    int rank = (ns == 1 || ns == 3) ? 0 : (ns == 0 ? 1 : 2);
                    if (!haveName || rank < (int)bestNs) {
                        haveName = true; bestNs = (u8)rank; e.ns = ns;
                        e.parent = pref;
                        e.name = W2U(name);
                        if (e.mftTime == 0) e.mftTime = mtime;
                    }
                }
            }
        } else if (type == 0x80) {
            if (nonRes) {
                u16 runOff = *(u16*)(rec + off + 0x20);
                u64 realSize = *(u64*)(rec + off + 0x30);
                if (runOff > 0 && off + runOff < off + alen) {
                    parseRunList(rec + off + runOff, alen - runOff, e.runs, 0);
                    e.dataSize = realSize;
                    e.allocSize = *(u64*)(rec + off + 0x28);
                    e.hasData = !e.runs.empty();
                }
            } else {
                u16 vlen = *(u16*)(rec + off + 0x10);
                u16 voff = *(u16*)(rec + off + 0x14);
                e.dataSize = vlen;
                e.hasData = true;
                if (vlen > 0 && (u32)voff + (u32)vlen <= alen) {
                    const u8* v = rec + off + voff;
                    e.resident.assign(v, v + vlen);
                }
            }
        }
        off += alen;
    }
    return haveName || e.inUse;
}

// ---------------------------------------------------------------------------
// 通配符匹配（大小写不敏感）
// ---------------------------------------------------------------------------
static bool wildMatch(const char* s, const char* p) {
    if (*p == 0) return *s == 0;
    if (*p == '*') {
        while (*p == '*') p++;
        if (*p == 0) return true;
        for (const char* t = s; ; t++) {
            if (wildMatch(t, p)) return true;
            if (*t == 0) return false;
        }
    }
    if (*s == 0) return false;
    if (*p == '?' || tolower((unsigned char)*s) == tolower((unsigned char)*p))
        return wildMatch(s + 1, p + 1);
    return false;
}

// ---------------------------------------------------------------------------
// 卷级扫描上下文
// ---------------------------------------------------------------------------
struct ScanCtx {
    Volume* vol = nullptr;
    vector<MftEntry> entries;   // 下标 = 记录号
    u64 recordCount = 0;
    vector<Run> mftRuns;
    u64 mftStreamSize = 0;
};

static bool loadMft(ScanCtx& ctx, string& err) {
    Volume& v = *ctx.vol;
    u8* rec = (u8*)malloc(v.mftRecordSize);
    if (!rec) { err = "内存不足"; return false; }
    if (!v.readAligned(v.mftLcn * v.clusterSize, rec, v.mftRecordSize)) {
        free(rec); err = "读取 $MFT 记录 0 失败"; return false;
    }
    MftEntry e0;
    if (!parseRecord(rec, v.mftRecordSize, v.bps, 0, e0) || !e0.hasData) {
        free(rec); err = "解析 $MFT 记录 0 失败"; return false;
    }
    ctx.mftRuns = e0.runs;
    ctx.mftStreamSize = e0.dataSize;
    ctx.recordCount = ctx.mftStreamSize / v.mftRecordSize;
    free(rec);
    if (ctx.recordCount == 0) { err = "$MFT 记录数为 0"; return false; }
    return true;
}

static bool scanMft(ScanCtx& ctx, string& err) {
    Volume& v = *ctx.vol;
    ctx.entries.clear();
    ctx.entries.resize((size_t)ctx.recordCount);

    const u32 batch = 1024;
    u32 bufBytes = batch * v.mftRecordSize;
    u8* buf = (u8*)VirtualAlloc(nullptr, bufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { err = "内存分配失败"; return false; }

    u64 done = 0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    g_lastPct = -1;

    for (u64 rec = 0; rec < ctx.recordCount; rec += batch) {
        u32 cnt = (u32)std::min<u64>(batch, ctx.recordCount - rec);
        u32 want = cnt * v.mftRecordSize;
        if (!v.readStream(ctx.mftRuns, rec * v.mftRecordSize, buf, want, ctx.mftStreamSize)) break;
        for (u32 i = 0; i < cnt; i++) {
            MftEntry e;
            if (parseRecord(buf + (size_t)i * v.mftRecordSize, v.mftRecordSize, v.bps, rec + i, e)) {
                ctx.entries[(size_t)(rec + i)] = std::move(e);
                g_parsed++;
            } else {
                g_failed++;
            }
        }
        done += want;
        QueryPerformanceCounter(&t1);
        double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        progress("MFT", done, ctx.mftStreamSize, secs, 0);
    }
    progressDone();
    VirtualFree(buf, 0, MEM_RELEASE);
    return true;
}

static string buildPath(const ScanCtx& ctx, u64 recno) {
    vector<string> parts;
    u64 cur = ctx.entries[(size_t)recno].parent;
    int guard = 0;
    while (cur != 0 && cur != 5 && cur < ctx.entries.size() && guard++ < 64) {
        const MftEntry& pe = ctx.entries[(size_t)cur];
        if (pe.name.empty()) break;
        parts.push_back(pe.name);
        cur = pe.parent;
    }
    string p;
    for (size_t i = parts.size(); i-- > 0;) { p += parts[i]; p += "\\"; }
    p += ctx.entries[(size_t)recno].name;
    return p;
}

// ---------------------------------------------------------------------------
// $Bitmap -> 空闲簇区间
// ---------------------------------------------------------------------------
static bool decodeBitmap(Volume& v, const MftEntry& be, vector<Run>& freeRuns, string& err) {
    u64 sz = be.dataSize;
    if (sz == 0 || be.runs.empty()) { err = "$Bitmap 元文件为空"; return false; }
    u8* bits = (u8*)malloc((size_t)sz + 4096);
    if (!bits) { err = "内存不足"; return false; }
    memset(bits, 0xFF, (size_t)sz);   // 读不到的部分按"已占用"处理，宁可少扫
    if (!v.readStream(be.runs, 0, bits, (u32)std::min<u64>(sz, 0x7FFFFFFF), sz)) {
        free(bits); err = "读取 $Bitmap 失败"; return false;
    }
    freeRuns.clear();
    u64 c = 0;
    while (c < v.totalClusters) {
        bool used = (bits[c >> 3] >> (c & 7)) & 1;
        if (used) { c++; continue; }
        u64 start = c;
        while (c < v.totalClusters && !((bits[c >> 3] >> (c & 7)) & 1)) c++;
        freeRuns.push_back(Run{ start, (i64)start, c - start });
    }
    free(bits);
    return true;
}

static bool loadFreeRuns(ScanCtx& ctx, vector<Run>& freeRuns, string& err) {
    int idx = -1;
    for (size_t i = 0; i < ctx.entries.size(); i++) {
        const MftEntry& e = ctx.entries[i];
        if (e.name == "$Bitmap" && e.parent == 5 && e.hasData && !e.runs.empty()) { idx = (int)i; break; }
    }
    if (idx < 0) { err = "找不到 $Bitmap（可能不是标准 NTFS）"; return false; }
    return decodeBitmap(*ctx.vol, ctx.entries[(size_t)idx], freeRuns, err);
}

// 快速路径：直接读第 6 号记录（$Bitmap），不遍历整个 MFT
static bool loadFreeRunsFast(ScanCtx& ctx, vector<Run>& freeRuns, string& err) {
    Volume& v = *ctx.vol;
    u32 recSize = v.mftRecordSize;
    u8* rec = (u8*)malloc(recSize);
    if (!rec) { err = "内存不足"; return false; }
    MftEntry e6;
    bool ok = v.readStream(ctx.mftRuns, 6ull * recSize, rec, recSize, ctx.mftStreamSize)
              && parseRecord(rec, recSize, v.bps, 6, e6)
              && e6.hasData && !e6.runs.empty();
    free(rec);
    if (!ok) { err = "$Bitmap 记录读取失败（记录号 6）"; return false; }
    return decodeBitmap(v, e6, freeRuns, err);
}

// ---------------------------------------------------------------------------
// 内容扫描
// ---------------------------------------------------------------------------
struct CarveOpt {
    vector<u8> pattern;
    bool  jsonMode = false;
    u32   contextKB = 128;
    u32   minSize = 32;
    u32   maxSize = 1024 * 1024;
    u64   limit = 0;
    string outDir;
    string volLetter;
};

static bool writeFileBytes(const string& path, const u8* data, u32 len) {
    HANDLE h = CreateFileW(U2W(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(h, data, len, &wr, nullptr) && wr == len;
    CloseHandle(h);
    return ok;
}

static const u8* memFind(const u8* hay, size_t n, const u8* pat, size_t m) {
    if (m == 0 || n < m) return nullptr;
    const u8* p = hay;
    size_t rem = n;
    while (rem >= m) {
        const u8* q = (const u8*)memchr(p, pat[0], rem - m + 1);
        if (!q) return nullptr;
        if (memcmp(q, pat, m) == 0) return q;
        rem -= (size_t)(q - p) + 1;
        p = q + 1;
    }
    return nullptr;
}

static bool carve(ScanCtx& ctx, const vector<Run>& scanRuns, const CarveOpt& opt, string& err) {
    Volume& v = *ctx.vol;
    u64 totalBytes = 0;
    for (auto& r : scanRuns) totalBytes += r.count * v.clusterSize;
    const u64 volBytes = v.clusterSize * v.totalClusters;   // 物理寻址上界

    const u32 CHUNK = 16u * 1024u * 1024u;
    u8* buf = (u8*)VirtualAlloc(nullptr, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { err = "内存分配失败"; return false; }

    const size_t patLen = opt.pattern.size();
    vector<u8> carry;      // 上一块末尾的 patLen-1 字节

    u64 done = 0, hits = 0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    g_lastPct = -1;

    for (size_t ri = 0; ri < scanRuns.size(); ri++) {
        const Run& r = scanRuns[ri];
        u64 bytes = r.count * v.clusterSize;
        u64 pos = 0;
        carry.clear();     // run 之间物理不连续，不能跨块拼接
        while (pos < bytes) {
            u32 want = (u32)std::min<u64>(CHUNK, bytes - pos);
            u64 phys = (u64)r.lcn * v.clusterSize + pos;
            // 注意：原始卷读要求偏移/长度/缓冲区都按扇区对齐，这里 buf 由 VirtualAlloc 给出，
            // 永远是页对齐的，绝不能把 carry 拼到 buf 前面再读。
            if (!v.readBytes(phys, buf, want)) { pos += want; done += want; continue; }
            done += want;

            if (!opt.jsonMode && patLen > 0) {
                auto handleHit = [&](u64 abs) -> bool {
                    hits++;
                    char line[256];
                    snprintf(line, sizeof line, "\n  命中 #%llu  偏移 0x%llX  空闲区 %llu\n",
                             (unsigned long long)hits, (unsigned long long)abs,
                             (unsigned long long)r.vcn);
                    out(line);
                    if (!opt.outDir.empty()) {
                        u32 ctxBytes = opt.contextKB * 1024u;
                        u64 st = abs > ctxBytes / 2 ? abs - ctxBytes / 2 : 0;
                        st &= ~511ull;
                        u64 en = std::min<u64>(abs + (u64)ctxBytes, volBytes);
                        if (en > st) {
                            u32 cb = (u32)std::min<u64>(en - st, (u64)ctxBytes);
                            cb &= ~511u;
                            if (cb) {
                                u8* cbuf = (u8*)malloc(cb);
                                if (cbuf && v.readBytes(st, cbuf, cb)) {
                                    char name[512];
                                    snprintf(name, sizeof name, "%s\\hit_%016llX.bin", opt.outDir.c_str(),
                                             (unsigned long long)abs);
                                    if (writeFileBytes(name, cbuf, cb)) out("  已保存上下文片段\n");
                                }
                                free(cbuf);
                            }
                        }
                    }
                    return !(opt.limit && hits >= opt.limit);
                };

                // 1) 块内匹配
                size_t idx = 0;
                while (idx + patLen <= (size_t)want) {
                    const u8* q = memFind(buf + idx, (size_t)want - idx, opt.pattern.data(), patLen);
                    if (!q) break;
                    size_t qIdx = (size_t)(q - buf);
                    if (!handleHit(phys + (u64)qIdx)) break;
                    idx = qIdx + 1;
                }

                // 2) 跨块边界：用单独的小窗口比对，不动 buf
                if (!carry.empty() && !(opt.limit && hits >= opt.limit)) {
                    size_t t = carry.size();
                    size_t take = std::min<size_t>((size_t)want, patLen - 1);
                    std::vector<u8> small(t + take);      // 不限定 pattern 长度
                    memcpy(small.data(), carry.data(), t);
                    memcpy(small.data() + t, buf, take);
                    size_t tot = t + take;
                    for (size_t sp = 0; sp + patLen <= tot; sp++) {
                        if (sp + patLen <= t) continue;   // 完全落在上一块，已经搜过
                        if (memcmp(small.data() + sp, opt.pattern.data(), patLen) != 0) continue;
                        u64 abs = phys - (u64)t + (u64)sp;
                        if (!handleHit(abs)) break;
                    }
                }

                // 3) 记录本块尾部，供下一块边界比对
                if (patLen > 1) {
                    size_t keep = std::min<size_t>((size_t)want, patLen - 1);
                    carry.assign(buf + want - keep, buf + want);
                }
            } else if (opt.jsonMode) {
                // JSON：匹配成对花括号（只处理完全落在本块内的对象）
                size_t i = 0;
                while (i < want) {
                    if (buf[i] != '{') { i++; continue; }
                    u32 depth = 0;
                    size_t j = i;
                    bool inStr = false, esc = false;
                    bool ok = false;
                    size_t lim = std::min<size_t>((size_t)want, i + (size_t)opt.maxSize);
                    for (; j < lim; j++) {
                        u8 c = buf[j];
                        if (inStr) {
                            if (esc) esc = false;
                            else if (c == '\\') esc = true;
                            else if (c == '"') inStr = false;
                            continue;
                        }
                        if (c == '"') inStr = true;
                        else if (c == '{') depth++;
                        else if (c == '}') {
                            depth--;
                            if (depth == 0) { ok = true; j++; break; }
                        }
                    }
                    if (ok && (j - i) >= opt.minSize && memchr(buf + i, ':', j - i)) {
                        hits++;
                        u64 abs = phys + i;
                        char line[256];
                        snprintf(line, sizeof line, "\n  JSON 对象 #%llu  偏移 0x%llX  大小 %llu B\n",
                                 (unsigned long long)hits, (unsigned long long)abs,
                                 (unsigned long long)(j - i));
                        out(line);
                        if (!opt.outDir.empty()) {
                            char name[512];
                            snprintf(name, sizeof name, "%s\\json_%016llX.json", opt.outDir.c_str(),
                                     (unsigned long long)abs);
                            if (writeFileBytes(name, buf + i, (u32)(j - i))) out("  已保存 JSON\n");
                        }
                        i = j;
                        if (opt.limit && hits >= opt.limit) break;
                    } else {
                        i++;
                    }
                }
            }

            pos += want;   // 块内偏移必须前进

            QueryPerformanceCounter(&t1);
            double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
            progress(opt.jsonMode ? "JSON" : "扫描", done, totalBytes, secs, hits);
            if (opt.limit && hits >= opt.limit) { pos = bytes; break; }
        }
        if (opt.limit && hits >= opt.limit) break;
    }
    progressDone();
    VirtualFree(buf, 0, MEM_RELEASE);

    char sum[128];
    snprintf(sum, sizeof sum, "\n完成：扫描 %s，命中 %llu 处\n",
             fmtSize(done).c_str(), (unsigned long long)hits);
    out(sum);
    return true;
}

// ---------------------------------------------------------------------------
// 帮助
// ---------------------------------------------------------------------------
static void help() {
    out(
    "StaDiskReset —— 磁盘已删除文件恢复 / 内容特征扫描\n"
    "版本 1.1.3   只读工具，不会修改源盘\n\n"
    "用法：\n"
    "  StaDiskReset --info   E:\n"
    "  StaDiskReset --list   E:  [--all] [--name \"*.json\"] [--limit 50]\n"
    "  StaDiskReset --recover E: --name \"hyw.json\" --out D:\\恢复\n"
    "  StaDiskReset --carve  E: --pattern \"ViewOnlyGamePlay\" --out D:\\恢复\n"
    "  StaDiskReset --carve  E: --extract json --out D:\\恢复\n\n"
    "参数：\n"
    "  --info            显示卷信息（簇大小、MFT 位置、记录数）\n"
    "  --list            列出 MFT 里的文件记录（默认只列已删除）\n"
    "  --recover        按 MFT 记录恢复文件内容到 --out 目录\n"
    "  --carve          扫描空闲簇，按内容特征找回\n"
    "  --name PAT        文件名过滤，支持 * 和 ?（默认 *）\n"
    "  --all             包含未删除的正常文件\n"
    "  --pattern STR     要搜索的特征字符串（UTF-8）\n"
    "  --pattern-hex HEX 用十六进制给特征，如 \"4A 53 4F 4E\"\n"
    "  --extract json    按 JSON 花括号结构提取\n"
    "  --context KB      命中时保存的上下文大小，默认 128\n"
    "  --min-size B      JSON 提取的最小字节数，默认 32\n"
    "  --max-size B      JSON 提取的最大字节数，默认 1048576\n"
    "  --whole           扫描整个卷（默认只扫空闲簇，快很多）\n"
    "  --out DIR         输出目录，必须位于其它磁盘\n"
    "  --limit N         命中/列表达到 N 条即停止\n"
    "  --help            显示本帮助\n\n"
    "示例：\n"
    "  StaDiskReset --list E: --name \"*.json\"\n"
    "  StaDiskReset --recover E: --name \"hyw.json\" --out D:\\out\n"
    "  StaDiskReset --carve E: --pattern \"ViewOnlyGamePlay\" --out D:\\out\n\n"
    "提示：必须以管理员身份运行；输出目录请不要放在要恢复的那块盘上。\n");
}

// ---------------------------------------------------------------------------
// 入口
// ---------------------------------------------------------------------------
static int runArgs(const vector<string>& args);
static int interactiveMenu();

static UINT g_oldOutCP = 0;
static UINT g_oldInCP = 0;

static void restoreCodePage() {
    if (g_oldOutCP) SetConsoleOutputCP(g_oldOutCP);
    if (g_oldInCP) SetConsoleCP(g_oldInCP);
}

int main() {
    // 中文不乱码：只改本控制台的代码页，退出时恢复（不用 system("chcp") 污染父 shell）
    g_oldOutCP = GetConsoleOutputCP();
    g_oldInCP = GetConsoleCP();
    atexit(restoreCodePage);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    g_tty = _isatty(_fileno(stdout)) != 0;

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argvW) { out("命令行解析失败\n"); return 1; }
    vector<string> args;
    for (int i = 1; i < argc; i++) args.push_back(W2U(argvW[i]));
    LocalFree(argvW);

    if (args.empty()) return interactiveMenu();
    return runArgs(args);
}

// ---------------------------------------------------------------------------
// 命令行模式
// ---------------------------------------------------------------------------
static int runArgs(const vector<string>& args) {
    string mode, volStr, outDir, namePat = "*", pattern, patternHex, extract;
    bool allFlag = false, whole = false;
    u64 limit = 0;
    u32 contextKB = 128, minSize = 32, maxSize = 1024 * 1024;

    for (size_t i = 0; i < args.size(); i++) {
        const string& a = args[i];
        auto need = [&](const char* what) -> string {
            if (i + 1 >= args.size()) { printf("参数 %s 缺少取值\n", what); exit(1); }
            return args[++i];
        };
        if (a == "--help" || a == "-h") { help(); return 0; }
        else if (a == "--info") mode = "info";
        else if (a == "--list") mode = "list";
        else if (a == "--recover") mode = "recover";
        else if (a == "--carve") mode = "carve";
        else if (a == "--all") allFlag = true;
        else if (a == "--whole") whole = true;
        else if (a == "--name") namePat = need("--name");
        else if (a == "--pattern") pattern = need("--pattern");
        else if (a == "--pattern-hex") patternHex = need("--pattern-hex");
        else if (a == "--extract") extract = need("--extract");
        else if (a == "--context") { long long v = _strtoi64(need("--context").c_str(), nullptr, 10); contextKB = (u32)(v > 0 ? v : 0); }
        else if (a == "--min-size") { long long v = _strtoi64(need("--min-size").c_str(), nullptr, 10); minSize = (u32)(v > 0 ? v : 0); }
        else if (a == "--max-size") { long long v = _strtoi64(need("--max-size").c_str(), nullptr, 10); maxSize = (u32)(v > 0 ? v : 0); }
        else if (a == "--limit") { long long v = _strtoi64(need("--limit").c_str(), nullptr, 10); limit = (u64)(v > 0 ? v : 0); }
        else if (a == "--out") outDir = need("--out");
        else if (a.size() >= 1 && a[0] != '-') { if (volStr.empty()) volStr = a; }
        else { printf("未知参数：%s\n", a.c_str()); return 1; }
    }

    if (mode.empty()) { help(); return 0; }

    // 卷号
    char L = 0;
    for (char c : volStr) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { L = (char)toupper((unsigned char)c); break; }
    }
    if (L == 0) { out("请指定卷，例如 E:\n"); return 1; }

    string err;
    Volume vol;
    if (!vol.open((wchar_t)L, err)) { printf("打开 %c: 失败：%s\n", L, err.c_str()); return 1; }

    printf("StaDiskReset 1.1.3   源卷 %c:\n", L);
    printf("  簇大小 %s   总簇数 %llu   容量 %s\n\n",
           fmtSize(vol.clusterSize).c_str(), (unsigned long long)vol.totalClusters,
           fmtSize(vol.clusterSize * vol.totalClusters).c_str());

    // 输出目录不能在同一块盘
    if (!outDir.empty()) {
        char oc = 0;
        for (char c : outDir) if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { oc = (char)toupper((unsigned char)c); break; }
        if (oc == L) { printf("拒绝执行：输出目录位于源卷 %c:，会把数据写回去\n", L); return 1; }
        CreateDirectoryW(U2W(outDir).c_str(), nullptr);
    }

    if (mode == "info") {
        printf("  分区起始 MFT LCN : %llu\n", (unsigned long long)vol.mftLcn);
        printf("  MFT 记录大小     : %u 字节\n", vol.mftRecordSize);
        printf("  每簇扇区数       : %u\n", vol.spc);
        printf("  每扇区字节数     : %u\n", vol.bps);
        return 0;
    }

    ScanCtx ctx;
    ctx.vol = &vol;
    out("正在读取 $MFT ...\n");
    if (!loadMft(ctx, err)) { printf("失败：%s\n", err.c_str()); return 1; }
    // carve 模式不需要遍历整个 MFT，只读第 6 号记录 $Bitmap 即可（秒开）
    if (mode != "carve") {
        if (!scanMft(ctx, err)) { printf("失败：%s\n", err.c_str()); return 1; }
        printf("MFT 记录 %llu 条，已解析 %llu 条，失败 %llu 条\n\n", (unsigned long long)ctx.recordCount, (unsigned long long)g_parsed, (unsigned long long)g_failed);
    }

    if (mode == "list" || mode == "recover") {
        u64 shown = 0;
        printf("%-8s %-7s %10s  %-19s  %s\n", "记录号", "状态", "大小", "修改时间", "路径");
        printf("--------------------------------------------------------------------------\n");
        for (size_t i = 1; i < ctx.entries.size(); i++) {
            const MftEntry& e = ctx.entries[i];
            if (e.name.empty()) continue;
            if (e.inUse && !allFlag) continue;
            if (!wildMatch(e.name.c_str(), namePat.c_str())) continue;
            string path = buildPath(ctx, e.recno ? e.recno : i);
            printf("%-8llu %-7s %10s  %-19s  %s\n",
                   (unsigned long long)i, e.inUse ? "正常" : "已删除",
                   fmtSize(e.dataSize).c_str(), fmtTime(e.mtime).c_str(), path.c_str());
            if (mode == "recover" && !e.isDir) {
                if (outDir.empty()) { out("\n恢复需要 --out 指定输出目录\n"); return 1; }
                bool isResident = !e.resident.empty();
                if (!isResident && (!e.hasData || e.runs.empty())) { out("  （无数据，跳过）\n"); }
                else {
                    u64 sz = e.dataSize;
                    wstring dir = U2W(outDir);
                    wstring baseName = U2W(e.name);
                    wstring outPath = dir + L"\\" + baseName;
                    // 重名自动改名：a.json -> a.1.json
                    for (int dup = 1; dup < 1000 && GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES; dup++) {
                        wchar_t suf[16];
                        swprintf(suf, 16, L".%d", dup);
                        size_t dot = baseName.rfind(L'.');
                        wstring cand = (dot == wstring::npos || dot == 0)
                                         ? baseName + suf
                                         : baseName.substr(0, dot) + suf + baseName.substr(dot);
                        outPath = dir + L"\\" + cand;
                    }
                    HANDLE fh = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
                    if (fh == INVALID_HANDLE_VALUE) {
                        printf("  写入失败（可能重名）：%s\n", W2U(outPath).c_str());
                    } else {
                        u64 written = 0;
                        if (isResident) {
                            DWORD wr = 0;
                            if (WriteFile(fh, e.resident.data(), (DWORD)e.resident.size(), &wr, nullptr))
                                written = wr;
                            sz = written;
                        } else {
                            u8* buf = (u8*)malloc((size_t)std::min<u64>(sz, 16ull << 20));
                            while (written < sz) {
                                u32 want = (u32)std::min<u64>(16ull << 20, sz - written);
                                if (!vol.readStream(e.runs, written, buf, want, sz)) break;
                                DWORD wr = 0;
                                if (!WriteFile(fh, buf, want, &wr, nullptr) || wr != want) break;
                                written += wr;
                            }
                            free(buf);
                        }
                        CloseHandle(fh);
                        if (sz > 0 && written < sz) {
                            printf("  ⚠ 部分恢复 %s / %s（%llu%%）-> %s\n",
                                   fmtSize(written).c_str(), fmtSize(sz).c_str(),
                                   (unsigned long long)(written * 100 / sz), W2U(outPath).c_str());
                        } else {
                            printf("  已恢复 %s -> %s%s\n", fmtSize(written).c_str(), W2U(outPath).c_str(),
                                   isResident ? "（常驻数据）" : "");
                        }
                    }
                }
            }
            shown++;
            if (limit && shown >= limit) break;
        }
        printf("\n共 %llu 条\n", (unsigned long long)shown);
        return 0;
    }

    if (mode == "carve") {
        CarveOpt opt;
        opt.outDir = outDir;
        opt.contextKB = contextKB;
        opt.minSize = minSize;
        opt.maxSize = maxSize;
        opt.limit = limit;
        char lc[4] = { L, ':', 0, 0 };
        opt.volLetter = lc;

        if (!extract.empty()) {
            if (extract != "json") { out("目前 --extract 只支持 json\n"); return 1; }
            opt.jsonMode = true;
        } else if (!patternHex.empty()) {
            string hex;
            for (char c : patternHex) if (isxdigit((unsigned char)c)) hex.push_back(c);
            if (hex.size() % 2) hex.pop_back();
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                opt.pattern.push_back((u8)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
            if (opt.pattern.empty()) { out("十六进制特征串解析失败\n"); return 1; }
        } else if (!pattern.empty()) {
            opt.pattern.assign(pattern.begin(), pattern.end());
        } else {
            out("请用 --pattern / --pattern-hex / --extract 指定要找的内容\n");
            return 1;
        }

        vector<Run> runs;
        if (whole) {
            runs.push_back(Run{ 0, 0, vol.totalClusters });
            out("扫描模式：整个卷（较慢）\n");
        } else {
            bool fastOk = loadFreeRunsFast(ctx, runs, err);
            if (!fastOk) {
                out("快速读取 $Bitmap 失败，回退到完整 MFT 扫描 ...\n");
                if (!scanMft(ctx, err)) { printf("失败：%s\n", err.c_str()); return 1; }
                if (!loadFreeRuns(ctx, runs, err)) { printf("失败：%s\n", err.c_str()); return 1; }
            }
            u64 fb = 0;
            for (auto& r : runs) fb += r.count * vol.clusterSize;
            printf("扫描模式：空闲簇 %llu 个（%s）\n", (unsigned long long)runs.size(), fmtSize(fb).c_str());
        }
        if (!carve(ctx, runs, opt, err)) { printf("失败：%s\n", err.c_str()); return 1; }
        return 0;
    }

    help();
    return 0;
}

/*
 *                       _ooOoo_
 *                      o8888888o
 *                      88" . "88
 *                      (| -_- |)
 *                      O\  =  /O
 *                   ____/`---'\____
 *                 .'  \\|     |//  `.
 *                /  \\|||  :  |||//  \
 *               /  _||||| -:- |||||-  \
 *               |   | \\\  -  /// |   |
 *               | \_|  ''\---/''  |   |
 *               \  .-\__  `-`  ___/-. /
 *             ___`. .'  /--.--\  `. . __
 *          ."" '<  `.___\_<|>_/___.'  >'"".
 *         | | :  `- \`.;`\ _ /`;.`/ - ` : | |
 *         \  \ `-.   \_ __\ /__ _/   .-` /  /
 *    ======`-.____`-.___\_____/___.-`____.-'======
 *                         `=---='
 *    ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *              佛祖保佑       永无BUG
 *               请勿修改       放飞自我
 *               别看了       看不懂      别改
 */

// 空转一下，意思意思（其实啥也不干）
void PIKAHA() {
    long long w = 0;
    for (int i = 0; i < 777; i++) {
        for (int j = 0; j < 777; j++) {
            w += (long long)i * j;
            int q = (int)(w % 631);
            q = q;
        }
    }
}

static string promptLine(const char* msg) {
    printf("%s", msg);
    fflush(stdout);
    string s;
    if (!std::getline(std::cin, s)) return string();
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

static string HEHE1() {
    char L = 0;
    cout << "请输入盘符(如E): ";
    cin >> L;
    cin.clear();
    cin.ignore(10000, '\n');
    for (char c : "ABCDEFGHIJKLMNOPQRSTUVWXYZ") { if (toupper(L) == c) { string v; v += c; v += ':'; return v; } }
    return "";
}

static string HEHE2(const char* def) {
    string s = promptLine("输出目录(回车默认 D:\\SDR_Recover): ");
    return s.empty() ? string(def) : s;
}

static int interactiveMenu() {
    int choice = 0;
    string vol;
TOP:
    system("cls");
    cout << "===============================================" << endl;
    cout << "        StaDiskReset 1.1.3  磁盘恢复工具箱" << endl;
    cout << "          双击即用 · 只读不写源盘" << endl;
    cout << "===============================================" << endl;
    cout << "1. 卷信息（簇大小 / MFT 位置）" << endl;
    cout << "2. 列出已删除的文件" << endl;
    cout << "3. 按文件名恢复" << endl;
    cout << "4. 按内容特征扫描（字符串）" << endl;
    cout << "5. 按 JSON 结构提取" << endl;
    cout << "6. 查看完整帮助" << endl;
    cout << "0. 退出程序" << endl;
    cout << "===============================================" << endl;
    cout << "请选择操作: ";
    cin >> choice;
    if (cin.fail()) { cin.clear(); cin.ignore(10000, '\n'); choice = -1; }
    else { cin.ignore(10000, '\n'); }

    if (choice == 1) {
        vol = HEHE1(); if (vol.empty()) { cout << "盘符不对吧" << endl; goto BYEBYE2; }
        { vector<string> a = { "--info", vol }; runArgs(a); }
        goto BYEBYE2;
    }
    if (choice == 2) {
        vol = HEHE1(); if (vol.empty()) { cout << "盘符不对吧" << endl; goto BYEBYE2; }
        {
            string name = promptLine("文件名过滤(支持 * ，回车=全部): ");
            vector<string> a = { "--list", vol };
            if (!name.empty()) { a.push_back("--name"); a.push_back(name); }
            a.push_back("--limit"); a.push_back("50");
            runArgs(a);
        }
        goto BYEBYE2;
    }
    if (choice == 3) {
        vol = HEHE1(); if (vol.empty()) { cout << "盘符不对吧" << endl; goto BYEBYE2; }
        {
            string name = promptLine("要恢复的文件名(支持 * ，如 hyw.json): ");
            if (name.empty()) { cout << "文件名不能为空" << endl; goto BYEBYE2; }
            string o = HEHE2("D:\\SDR_Recover");
            vector<string> a = { "--recover", vol, "--name", name, "--out", o };
            runArgs(a);
        }
        goto BYEBYE2;
    }
    if (choice == 4) {
        vol = HEHE1(); if (vol.empty()) { cout << "盘符不对吧" << endl; goto BYEBYE2; }
        {
            string pat = promptLine("要搜索的特征字符串: ");
            if (pat.empty()) { cout << "特征串不能为空" << endl; goto BYEBYE2; }
            string o = HEHE2("D:\\SDR_Recover");
            vector<string> a = { "--carve", vol, "--pattern", pat, "--out", o, "--limit", "20" };
            runArgs(a);
        }
        goto BYEBYE2;
    }
    if (choice == 5) {
        vol = HEHE1(); if (vol.empty()) { cout << "盘符不对吧" << endl; goto BYEBYE2; }
        {
            string o = HEHE2("D:\\SDR_Recover");
            string mn = promptLine("最小字节数(回车默认 1024): ");
            if (mn.empty()) mn = "1024";
            vector<string> a = { "--carve", vol, "--extract", "json", "--min-size", mn, "--out", o, "--limit", "20" };
            runArgs(a);
        }
        goto BYEBYE2;
    }
    if (choice == 6) {
        system("cls");
        help();
        goto BYEBYE2;
    }
    if (choice == 0) goto BYEBYE;

    cout << "无效选择，请重新输入" << endl;
    PIKAHA();
    Sleep(300);
    goto TOP;

BYEBYE2:
    cout << endl << "按任意键继续..." << endl;
    _getch();
    goto TOP;

BYEBYE:
    cout << "感谢使用，再见!" << endl;
    cout << "按任意键退出" << endl;
    _getch();
    return 0;
}