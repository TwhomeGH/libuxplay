# libuxplay 變更歷史

本歷史自 2026-11-22 建立，不回填成未經查證的舊版紀錄。
建立前的程式狀態基線為本機提交 `132a933`；該提交不代表所有實機驗收完成。
## 2026.11.22 01:56 列出引擎註釋維護規則與補齊順序

**類型**: 文件

### 問題 - 底層標頭與複雜實作尚未完整說明單位、執行緒與資源契約

### 修改 - 依音訊緩衝、RTP／NTP、audio renderer、mirror／video renderer 與 virtual camera 排定補齊順序

- 修改時同步維護中文 Doxygen 與設計原因，不將未驗證的同步保證寫成已完成事實

**相關文件**: TODO.md

---

## 2026.11.22 01:48 將引擎 API 掛載至獨立 changelogkit 入口

**類型**: 新增

### 問題 - 引擎 changelogkit 缺少可讀取的 API HTML 入口

### 根因 - Doxygen 產物未設定 API provider，原輸出目錄又與靜態匯出重疊

### 修改 - 新增 libuxplay API provider，使用 build/doxygen/libuxplay 並啟用 embed

獨立動態掛載與靜態 api-docs/libuxplay/index.html 已驗證；changelogkit 只掛載 HTML，不宣稱新增 tagfile 符號 adapter。

- 將 Doxygen 來源與 _site 匯出目的地分開，保留獨立引擎文件建置能力

**相關文件**:

- .changelog.toml
- docs/API-GUIDE.md

---

## 2026.11.22 01:33 建立獨立 API 參考與變更歷史入口

**類型**: 新增

### 問題 - 引擎缺少獨立的 API 文件與變更歷史，宿主文件未涵蓋 RTP 與 NTP 核心

### 根因 - 文件原本集中在主 App，子模組未建立 Doxygen 與 changelogkit 設定

### 修改 - 新增 libuxplay Doxyfile、API 導讀、公開 C 介面參數與生命週期註解，輸出可供宿主引用的標籤檔

Doxygen 本機建置零警告；核心標頭索引與宿主交叉連結已檢查。底層既有標頭的完整參數註解、計數器同步及實機串流驗收仍列待辦。

- 新增子模組 changelogkit 設定、變更歷史與待辦；以 132a933 為建立前基線，不捏造舊版歷史

**相關文件**:

- docs/API-GUIDE.md
- TODO.md

---
