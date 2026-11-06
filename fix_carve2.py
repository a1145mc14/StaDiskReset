import io

p = r'D:\StaDiskReset\StaDiskReset.cpp'
s = io.open(p, encoding='utf-8').read().replace('\r\n', '\n')

start = s.index('static bool carve(ScanCtx& ctx, const vector<Run>& scanRuns, const CarveOpt& opt, string& err) {')
end_marker = '    out(sum);\n    return true;\n}'
end = s.index(end_marker, start) + len(end_marker)

new_carve = r'''static bool carve(ScanCtx& ctx, const vector<Run>& scanRuns, const CarveOpt& opt, string& err) {
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
                    u8 small[160];
                    size_t t = carry.size();
                    size_t take = std::min<size_t>((size_t)want, patLen - 1);
                    if (t + take <= sizeof(small)) {
                        memcpy(small, carry.data(), t);
                        memcpy(small + t, buf, take);
                        size_t tot = t + take;
                        for (size_t sp = 0; sp + patLen <= tot; sp++) {
                            if (sp + patLen <= t) continue;   // 完全落在上一块，已经搜过
                            if (memcmp(small + sp, opt.pattern.data(), patLen) != 0) continue;
                            u64 abs = phys - (u64)t + (u64)sp;
                            if (!handleHit(abs)) break;
                        }
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
}'''

s = s[:start] + new_carve + s[end:]
io.open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('carve rewritten, bytes =', len(s))
