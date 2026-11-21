# libuxplay API 參考 {#mainpage}

這份參考屬於 libuxplay 子模組，涵蓋宿主 C API、AirPlay／RTP 核心標頭與
音訊／影片 renderer 標頭。Qt 主視窗、設定、托盤與背景日誌由主 App 維護。

整合目錄中的 <a href="../app/index.html">uxplay-windows App API</a>
提供宿主端的模組地圖；單獨 checkout libuxplay 時，該相對連結可能不存在。

## 介面分層

| 層級 | 標頭／入口 | 用途 |
| --- | --- | --- |
| 宿主 C API | uxplay_api.h | 啟動、停止、事件回呼與診斷計數 |
| AirPlay server | lib/raop.h、lib/raop_rtp.h | 連線、音訊 RTP、封包計數 |
| 音訊緩衝與時間 | lib/raop_buffer.h、lib/raop_ntp.h | 序號、補包、時限與時鐘換算 |
| Mirror | lib/raop_rtp_mirror.h | 影片接收與生命週期 |
| Renderer | renderers/audio_renderer.h、renderers/video_renderer.h | GStreamer 生命週期與觀測點 |
| Virtual camera | renderers/virtual_camera_writer.h | Windows frame pipe bridge |

`lib/playfair` 與 `lib/llhttp` 是內嵌相依實作，不列入這套維護介面。
底層標頭以符號索引為主，既有上游宣告尚未全部補齊參數文件；宿主 C API
則記錄用途、參數、回傳值與重要限制。文件不代表穩定 ABI 承諾。

## 宿主生命週期

1. 先註冊回呼，再由工作執行緒呼叫 start_uxplay()；此函式會阻塞。
2. ready 回呼表示引擎初始化已到達通知點，不代表已有裝置串流或實際出聲。
3. UI 或控制執行緒呼叫 stop_uxplay() 發出停止要求。
4. 等待 start_uxplay() 返回、工作執行緒結束後，再解除回呼或釋放宿主物件。

回呼可能從不同引擎／媒體執行緒發出。字串是暫借資料，需保留時應立即複製；
Qt 宿主應將 UI 操作排入 UI 執行緒。不要在媒體回呼內同步寫檔、更新硬體
探測、等待 UI 或重新啟動引擎。一次只支援一個程序全域引擎實例。

計數器是診斷觀測值，目前多數沒有跨欄位快照同步保護；不得把它們當成
一致的交易快照或已驗證的無資料競爭介面。設定回呼及 reset 應在引擎停止時操作。

## 音訊數據的意義

- `decoded` 是歷史參數名稱，實際表示 RTP payload 已出列，並非 AAC 解碼成功。
- `pushed` 表示 appsrc 接受 payload，不代表音效裝置已播放。
- `peak` 是 level 元素的 dBFS 峰值；尚無觀測時初值為 -1，不能只靠它判斷是否靜音。
- 音訊 age 使用 `UINT64_MAX` 表示尚無資料，顯示端應轉成「尚無輸出資料」。
- 影片 age 使用負值表示尚無可用觀測，不應與音訊的整數哨兵值混用。
- `gap_skips` 計算跳過的缺失序號；明確無內容封包不算缺包。
- `audio ingress` 與 `audio delivery` 是每秒 log 診斷，映射時間戳落後量可能
  同時包含傳送端、網路與時鐘映射誤差；`queue_wait_ms` 只量本機緩衝停留。

## 文件與歷史維護

在 libuxplay 根目錄先建立 `build/doxygen` 父目錄，再執行 `doxygen Doxyfile`，
輸出 `build/doxygen/libuxplay/index.html`
及供 App 交叉連結使用的 `libuxplay.tag`。產物不提交。

本子模組的 `[[tool.changelog.api]]` 掛載 `build/doxygen/libuxplay`，明確
設定 `embed = true`。在本目錄啟動 changelogkit 即有「libuxplay API」入口；
先產生文件再匯出，靜態站會有 `_site/api-docs/libuxplay/index.html`。
`_site` 是匯出目錄，不能用來保存 Doxygen 來源。獨立引擎站沒有 App 掛載，
因此只有在主 App 共同掛載兩站時，跨至 App 的連結才可使用。

`.changelog.toml` 維護本子模組的 `docs/ChangeHistory.md` 與 `TODO.md`。
在本目錄執行 `python -m changelogkit add ...`；主 App 的歷史只記錄宿主
行為、整合與採用的子模組版本，不取代底層歷史。若工具未安裝，沿用開發
環境既有 changelogkit 安裝或 PYTHONPATH，不在設定內綁定個人電腦路徑。

這次建立文件基線，歷史不追溯捏造舊提交日期。仍待完成的驗收見 TODO.md。
