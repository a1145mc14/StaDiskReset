# StaDiskReset

NTFS 已删除文件恢复 / 内容特征扫描 —— 单文件命令行工具，**只读，不写源盘**。

```
StaDiskReset.exe        668 KB，静态链接，无需安装任何运行库
StaDiskReset.cpp        单文件源码，g++ 直接可编
```

---

## 为什么会有这个工具

UMM（UnityModManager）卸载 mod 时会**整目录删除**，`Mods\KeyViewer\profiles\hyw.json` 就是这样被带走的。
这个工具把当时那套抢救流程固化成了命令：先按文件名翻 MFT，翻不到再按内容特征去空闲簇里挖。

---

## 快速开始（必须用管理员身份运行）

```bat
:: 1. 看卷信息
StaDiskReset --info E:

:: 2. 列出已删除的文件（按名字过滤，支持 * 和 ?）
StaDiskReset --list E: --name "*.json"

:: 3. 按名字恢复内容到别的盘
StaDiskReset --recover E: --name "hyw.json" --out D:\恢复

:: 4. 按内容特征找（记得住里面的一句话/一个字段时用这个）
StaDiskReset --carve E: --pattern "ViewOnlyGamePlay" --out D:\恢复

:: 5. 按 JSON 结构整块提取（不在乎原文件名时用这个）
StaDiskReset --carve E: --extract json --min-size 50000 --out D:\恢复
```

---

## 交互模式（双击即用）

不带任何参数双击运行，进入数字菜单，输入 `1`～`6` 选择功能，`0` 退出：

```
  ==================================================
   StaDiskReset 1.1.0   磁盘恢复工具箱
  ==================================================
    [1] 卷信息（簇大小 / MFT 位置）
    [2] 列出已删除的文件
    [3] 按文件名恢复
    [4] 按内容特征扫描（字符串）
    [5] 按 JSON 结构提取
    [6] 查看完整帮助
    [0] 退出
  --------------------------------------------------
  请输入序号:
```

每项操作只需回答「盘符 / 文件名 / 输出目录」，其余参数走默认值，适合不想记命令行参数时使用。
## 修复记录（v1.1.2）

- **上下文片段偏移截断**：`end` 上界改用 `cliusterSize * totalClusters`，碎片化空闲区不再下溢
- **跨块特征匹配**：保留上一块尾部 `patLen-1` 字节做边界比对，特征串横跨 16 MB 块也能命中
- **原始卷读对齐**：缓冲区必须扇区对齐，之前把 carry 拼到 `buf+16` 导致 `ERROR_INVALID_PARAMETER(87)`、读取被静默跳过（表现为"秒扫全盘 0 命中"）
- **块内偏移前进**：补回缺失的 `pos += want`，否则永远停在第一块
- **同名文件**：`--recover` 改用 `CREATE_NEW` + 自动改名 `a.1.json`，不再互相覆盖
- **常驻数据**：`< 700 B` 的小文件数据在 MFT 记录里，现已直接还原（不再被跳过）
- **run 查找**：`readStream` 用 `std::upper_bound` 替代线性扫，高度碎片化文件恢复更快
- **`--pattern-hex`**：先去非 hex 字符再两位一组，`4A53` 和 `4A 53` 都能正确解析
## 修复记录（v1.1.3）

- **`--limit` 只出一半**：`shown` 在循环里自增两次，`--limit 50` 实际只列 25 条
- **`$STANDARD_INFORMATION` 越界读**：`vlen >= 16` 却读到 `v+23`，改为 `>= 24`
- **被截断的恢复不报警**：`readStream` 中途失败时仍打印「已恢复」，现在会标「⚠ 部分恢复 x / y（百分比）」
- **长 pattern 跨块静默跳过**：边界窗口原来用定长 `u8[160]`，`patLen > 81` 时整段跳过，改为 `std::vector`
- **数值参数负数**：`atoi("-1")` 转 `u32` 变成 4294967295，改用 `_strtoi64` + 负值钳 0
- **不再污染父 shell 代码页**：去掉 `system("chcp 65001")`，改用 `SetConsoleOutputCP` 并在退出时 `atexit` 恢复原 CP
- **`readBytes` 长度溢出**：`pre + len + 511` 在极端长度下会溢出 `u32`，加了前置检查
## 命令一览

| 参数 | 说明 |
|---|---|
| `--info` | 卷信息：簇大小、MFT 位置、记录大小 |
| `--list` | 列 MFT 记录，默认只列**已删除**的；加 `--all` 连正常的也列 |
| `--recover` | 从 MFT 记录的数据 run 直接读出内容 |
| `--carve` | 扫空闲簇：`--pattern` 特征串 / `--extract json` 结构提取 |
| `--name PAT` | 文件名过滤，支持 `*` `?`，默认 `*` |
| `--pattern STR` | 要搜的字符串（UTF-8） |
| `--pattern-hex HEX` | 十六进制特征，如 `"4A 53 4F 4E"` |
| `--extract json` | 按成对花括号提取 JSON 对象 |
| `--context KB` | 命中时保存的上下文大小，默认 128 |
| `--min-size` / `--max-size` | JSON 提取的字节数上下限 |
| `--whole` | 扫整个卷（默认只扫空闲簇，快 5–10 倍） |
| `--limit N` | 命中/列表达 N 条即停 |
| `--out DIR` | 输出目录，**必须在别的磁盘上** |

---

## 实测性能（kimtigo 512GB SATA SSD，i5 级别机器）

| 操作 | 数据量 | 耗时 | 速度 |
|---|---|---|---|
| `--info` | — | 立即 | — |
| MFT 全量解析（D:，83 万条记录） | 811 MB | ~4 s | — |
| `--carve` 空闲簇扫描（E:，18.1 GB） | 18.1 GB | ~66 s | 275 MB/s |
| 磁盘顺序读上限 | — | — | 362 MB/s |

> carve 模式**不解析整个 MFT**，直接读第 6 号记录 `$Bitmap` 拿空闲图，启动几乎无延迟。

---

## 验证状态（实话实说）

| 功能 | 状态 | 依据 |
|---|---|---|
| MFT 解析 / 路径还原 | ✅ 已验证 | D: 830,976 条记录，成功 830,403 条 |
| `--recover` 按名恢复 | ✅ 已验证 | 19,764 B 文件恢复后 **SHA256 与原文件完全一致** |
| `--list` 已删除过滤 | ✅ 已验证 | 能列出 `314650 已删除 ... manifest.json` 等历史删除记录 |
| `--carve` 特征匹配 | ⚠️ 未在真实数据上验到正向命中 | 两次实测 0 命中，原因是源盘 SSD 的 TRIM 已把删除内容抹零，**不是匹配器的错** —— 用 `--whole` 对已分配区扫描可复现正向命中 |
| `--carve --extract json` | ⚠️ 同上 | 同上 |

> 想快速自测匹配器：拿一个**没删**的文件，`--carve <卷> --pattern "<文件里的独有字符串>" --whole --limit 1`，秒出结果。

---

## 已知限制

1. **只支持 NTFS**（FAT32 / exFAT / ReFS 不支持）。
2. **必须管理员权限**，否则打不开 `\\.\X:` 卷句柄。
3. 输出目录不能和源盘同一个盘符，程序会直接拒绝（防止写回去覆盖证据）。
4. **SSD + TRIM 是硬伤**：删除后主控会懒擦除，越晚动手越没戏。机械盘 / TRIM 关闭的 SSD 存活率高得多。
5. MFT 被复用的删除记录，文件名会先丢；这时只能靠 `--carve` 按内容找。
6. JSON 提取只处理**完整落在同一个 16 MB 块内**的对象，跨块的会被跳过。

---

## 从源码编译

```bat
g++ -std=c++17 -O2 -static -fexec-charset=UTF-8 -o StaDiskReset.exe StaDiskReset.cpp -lshell32
```

MSVC 也可以，把 `CommandLineToArgvW` 需要的 `shell32.lib` 带上即可。

---

## 实现要点（方便你改）

- **MFT**：读引导扇区拿 `MFT LCN` / 记录大小 → 解析记录 0 的 `$DATA` run 表 → 顺序流式读整条 `$MFT`，每 1024 条一批，套用 fixup 后解析 `$STANDARD_INFORMATION`(0x10) / `$FILE_NAME`(0x30) / `$DATA`(0x80)。
- **runlist**：注意 NTFS 的 offset 是**相对上一个 run 的增量**，必须累加（这里踩过坑）。
- **空闲图**：直接读记录 6 `$Bitmap`，bit=1 表示占用；合并连续 0 位成 run 区间。
- **扇区对齐**：原始卷的 `ReadFile` 要求**偏移和长度都是 512 的倍数**，否则 `ERROR_INVALID_PARAMETER(87)`。代码里 `readBytes()` 自动做前后对齐 + 尾部拷贝。
- **定位读**：用 `OVERLAPPED` 带偏移的 `ReadFile`，省掉每段一次 `SetFilePointerEx`（空闲区碎片多时提速 60%）。
- **中文不乱码**：`system("chcp 65001")` + `SetConsoleOutputCP(CP_UTF8)`，源码以 UTF-8 保存并用 `-fexec-charset=UTF-8` 编译。

---

## 许可

MIT License，见 [LICENSE](LICENSE)。

- 作者：**C艹是adofai**
- 说明：AI 辅助生成的部分不主张版权，本项目整体以 MIT 协议发布
- 起因：为了抢救被 UnityModManager 卸载掉的 KeyViewer 皮肤档案

使用者自行承担数据风险 —— 这是**只读**工具，但恢复结果不保证完整（详见「已知限制」）。

