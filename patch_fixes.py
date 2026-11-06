import io, sys

p = r'D:\StaDiskReset\StaDiskReset.cpp'
s = io.open(p, encoding='utf-8').read().replace('\r\n', '\n')
orig_len = len(s)
report = []

def rep(old, new, tag):
    global s
    if old not in s:
        report.append(f'[MISS] {tag}')
        return
    s = s.replace(old, new, 1)
    report.append(f'[OK]   {tag}')

# ---------------- 4) readStream: 线性查找 -> upper_bound ----------------
rep('''            const Run* r = nullptr;
            for (size_t i = 0; i < runs.size(); i++)
                if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].count) { r = &runs[i]; break; }
            if (!r) return false;''',
'''            auto it = std::upper_bound(runs.begin(), runs.end(), vcn,
                                       [](u64 v, const Run& rr) { return v < rr.vcn; });
            if (it == runs.begin()) return false;
            --it;
            if (vcn >= it->vcn + it->count) return false;
            const Run* r = &(*it);''', '4) readStream upper_bound')

# ---------------- 5a) MftEntry 增加 resident ----------------
rep('''    u8   ns = 0;
    string name;
    vector<Run> runs;''',
'''    u8   ns = 0;
    string name;
    vector<Run> runs;
    vector<u8> resident;      // 常驻属性数据（小文件就在 MFT 记录里）''', '5a) MftEntry::resident')

# ---------------- 5b) parseRecord 保存常驻数据 ----------------
rep('''            } else {
                u16 vlen = *(u16*)(rec + off + 0x10);
                e.dataSize = vlen;
                e.hasData = true;
            }''',
'''            } else {
                u16 vlen = *(u16*)(rec + off + 0x10);
                u16 voff = *(u16*)(rec + off + 0x14);
                e.dataSize = vlen;
                e.hasData = true;
                if (vlen > 0 && (u32)voff + (u32)vlen <= alen) {
                    const u8* v = rec + off + voff;
                    e.resident.assign(v, v + vlen);
                }
            }''', '5b) 常驻数据入 MftEntry')

# ---------------- 6) --pattern-hex 解析 ----------------
rep('''        } else if (!patternHex.empty()) {
            const char* s = patternHex.c_str();
            while (*s) {
                while (*s == ' ' || *s == ',' || *s == '-') s++;
                if (!*s) break;
                char b[3] = { s[0], (char)((s[1] && s[1] != ' ') ? s[1] : 0), 0 };
                if (!b[1]) break;
                opt.pattern.push_back((u8)strtol(b, nullptr, 16));
                s += 2;
            }''',
'''        } else if (!patternHex.empty()) {
            string hex;
            for (char c : patternHex) if (isxdigit((unsigned char)c)) hex.push_back(c);
            if (hex.size() % 2) hex.pop_back();
            for (size_t i = 0; i + 1 < hex.size(); i += 2)
                opt.pattern.push_back((u8)strtol(hex.substr(i, 2).c_str(), nullptr, 16));
            if (opt.pattern.empty()) { out("十六进制特征串解析失败\\n"); return 1; }''', '6) hex 解析')

# ---------------- 1)+2) carve 整个函数重写 ----------------
start = s.index('static bool carve(ScanCtx& ctx, const vector<Run>& scanRuns, const CarveOpt& opt, string& err) {')
end_marker = '    out(sum);\n    return true;\n}'
end = s.index(end_marker, start) + len(end_marker)

new_carve = r'''static bool carve(ScanCtx& ctx, const vector<Run>& scanRuns, const CarveOpt& opt, string& err) {
    Volume& v = *ctx.vol;
    u64 totalBytes = 0;
    for (auto& r : scanRuns) totalBytes += r.count * v.clusterSize;
    const u64 volBytes = v.clusterSize * v.totalClusters;   // 物理寻址上界（不是空闲区总和）

    const u32 CHUNK = 16u * 1024u * 1024u;
    u8* buf = (u8*)VirtualAlloc(nullptr, (size_t)CHUNK + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) { err = "内存分配失败"; return false; }

    const size_t patLen = opt.pattern.size();
    vector<u8> carry;    // 上一块末尾的 patLen-1 字节，用于跨块匹配

    u64 done = 0, hits = 0;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    g_lastPct = -1;

    for (size_t ri = 0; ri < scanRuns.size(); ri++) {
        const Run& r = scanRuns[ri];
        u64 bytes = r.count * v.clusterSize;
        u64 pos = 0;
        carry.clear();   // run 之间物理不连续，不允许跨块拼接
        while (pos < bytes) {
            u32 want = (u32)std::min<u64>(CHUNK, bytes - pos);
            u64 phys = (u64)r.lcn * v.clusterSize + pos;
            size_t tailLen = opt.jsonMode ? 0 : carry.size();
            u8* data = buf + tailLen;
            if (!v.readBytes(phys, data, want)) { pos += want; done += want; continue; }
            done += want;
            size_t total = tailLen + want;
            u64 dataAbs = phys - (u64)tailLen;   // buf[0] 对应的物理偏移

            if (!opt.jsonMode && patLen > 0) {
                // 起点只从"上一块没搜过的位置"开始，避免重复命中
                size_t idx = (tailLen + 1 >= patLen) ? (tailLen - (patLen - 1)) : 0;
                while (idx + patLen <= total) {
                    const u8* q = memFind(buf + idx, total - idx, opt.pattern.data(), patLen);
                    if (!q) break;
                    size_t qIdx = (size_t)(q - buf);
                    u64 abs = dataAbs + (u64)qIdx;
                    hits++;
                    char line[256];
                    snprintf(line, sizeof line, "\n  命中 #%llu  偏移 0x%llX  空闲区 %llu\n",
                             (unsigned long long)hits, (unsigned long long)abs,
                             (unsigned long long)r.vcn);
                    out(line);
                    if (!opt.outDir.empty()) {
                        u32 ctxBytes = opt.contextKB * 1024u;
                        u64 start = abs > ctxBytes / 2 ? abs - ctxBytes / 2 : 0;
                        start &= ~511ull;
                        u64 end = std::min<u64>(abs + (u64)ctxBytes, volBytes);
                        if (end > start) {
                            u32 cb = (u32)std::min<u64>(end - start, (u64)ctxBytes);
                            cb &= ~511u;
                            if (cb) {
                                u8* cbuf = (u8*)malloc(cb);
                                if (cbuf && v.readBytes(start, cbuf, cb)) {
                                    char name[512];
                                    snprintf(name, sizeof name, "%s\\hit_%016llX.bin", opt.outDir.c_str(),
                                             (unsigned long long)abs);
                                    if (writeFileBytes(name, cbuf, cb)) out("  已保存上下文片段\n");
                                }
                                free(cbuf);
                            }
                        }
                    }
                    idx = qIdx + 1;
                    if (opt.limit && hits >= opt.limit) break;
                }
            } else if (opt.jsonMode) {
                // JSON：匹配成对花括号（只处理完全落在本块内的对象）
                size_t i = 0;
                while (i < want) {
                    if (data[i] != '{') { i++; continue; }
                    u32 depth = 0;
                    size_t j = i;
                    bool inStr = false, esc = false;
                    bool ok = false;
                    size_t lim = std::min<size_t>(want, i + (size_t)opt.maxSize);
                    for (; j < lim; j++) {
                        u8 c = data[j];
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
                    if (ok && (j - i) >= opt.minSize && memchr(data + i, ':', j - i)) {
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
                            if (writeFileBytes(name, data + i, (u32)(j - i))) out("  已保存 JSON\n");
                        }
                        i = j;
                        if (opt.limit && hits >= opt.limit) break;
                    } else {
                        i++;
                    }
                }
            }

            if (!opt.jsonMode && patLen > 1) {
                size_t keep = std::min<size_t>(total, patLen - 1);
                carry.assign(buf + total - keep, buf + total);
            }

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
}'''
s = s[:start] + new_carve + s[end:]
report.append('[OK]   1)+2) carve 重写（物理偏移上界 / 跨块匹配）')

# ---------------- 3)+5c) --recover 改名 + 常驻数据 ----------------
old_rec = '''            if (mode == "recover" && !e.isDir) {
                if (outDir.empty()) { out("\\n恢复需要 --out 指定输出目录\\n"); return 1; }
                if (!e.hasData || e.runs.empty()) { out("  （无数据，跳过）\\n"); }
                else {
                    u64 sz = e.dataSize;
                    u8* buf = (u8*)malloc((size_t)std::min<u64>(sz, 16ull << 20));
                    u64 written = 0;
                    wstring outPath = U2W(outDir) + L"\\\\" + U2W(e.name);
                    HANDLE fh = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
                    if (fh == INVALID_HANDLE_VALUE) {
                        printf("  写入失败（可能重名）：%s\\n", W2U(outPath).c_str());
                    } else {
                        while (written < sz) {
                            u32 want = (u32)std::min<u64>(16ull << 20, sz - written);
                            if (!vol.readStream(e.runs, written, buf, want, sz)) break;
                            DWORD wr = 0;
                            if (!WriteFile(fh, buf, want, &wr, nullptr) || wr != want) break;
                            written += wr;
                        }
                        CloseHandle(fh);
                            printf("  已恢复 %s -> %s\\n", fmtSize(written).c_str(), W2U(outPath).c_str());
                    }
                    free(buf);
                }
            }'''

new_rec = '''            if (mode == "recover" && !e.isDir) {
                if (outDir.empty()) { out("\\n恢复需要 --out 指定输出目录\\n"); return 1; }
                bool isResident = !e.resident.empty();
                if (!isResident && (!e.hasData || e.runs.empty())) { out("  （无数据，跳过）\\n"); }
                else {
                    u64 sz = e.dataSize;
                    wstring dir = U2W(outDir);
                    wstring baseName = U2W(e.name);
                    wstring outPath = dir + L"\\\\" + baseName;
                    // 重名自动改名：a.json -> a.1.json
                    for (int dup = 1; dup < 1000 && GetFileAttributesW(outPath.c_str()) != INVALID_FILE_ATTRIBUTES; dup++) {
                        wchar_t suf[16];
                        swprintf(suf, 16, L".%d", dup);
                        size_t dot = baseName.rfind(L'.');
                        wstring cand = (dot == wstring::npos || dot == 0)
                                         ? baseName + suf
                                         : baseName.substr(0, dot) + suf + baseName.substr(dot);
                        outPath = dir + L"\\\\" + cand;
                    }
                    HANDLE fh = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
                    if (fh == INVALID_HANDLE_VALUE) {
                        printf("  写入失败（可能重名）：%s\\n", W2U(outPath).c_str());
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
                        printf("  已恢复 %s -> %s%s\\n", fmtSize(written).c_str(), W2U(outPath).c_str(),
                               isResident ? "（常驻数据）" : "");
                    }
                }
            }'''
rep(old_rec, new_rec, '3)+5c) --recover 改名 + 常驻')

io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('\n'.join(report))
print(f'size: {orig_len} -> {len(s)} bytes')
