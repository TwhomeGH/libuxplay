# libuxplay 待辦

本清單記錄引擎層工作；Qt UI、設定與宿主整合驗收由主 App 的 TODO.md 維護。

## 持續規則 — 改哪裡，補哪裡

每次修改引擎函式時，同步補齊對應標頭契約與非直觀實作的原因，以中文說明。
公開介面使用 Doxygen 註釋，記錄參數、回傳、單位、未知值、資料擁有權、
呼叫執行緒與生命週期；封包格式則註明序號／時間戳、位元組序與合法空封包。
時限、補包、時鐘換算及相容處理應解釋取捨，保留上游出處，不逐行翻譯程式。

註釋只能記錄實作已支持的契約；尚未同步的診斷計數器不得寫成 thread-safe。
行為變更時同步修正舊說明。未涉及的舊程式與內嵌第三方程式不做批次註釋改寫。

每項完成前核對實作與呼叫處，確認標頭與來源一致、參數／單位／限制明確，
並重建相關 API 文件。Doxygen 零警告只驗證可產生，不代表既有 API 全部已說明。

## 既有程式補齊順序

- [ ] 優先：`lib/raop_buffer.h/.c` — 序號回繞、空封包、缺口等待、補包節流與 payload 擁有權。
- [ ] 優先：`lib/raop_rtp.h/.c`、`lib/raop_ntp.h/.c` — 時鐘來源／單位、映射意義、回呼執行緒與停止回收。
- [ ] 優先：`renderers/audio_renderer.h/.c` — appsrc、level、實際播放的區別，flush／restart 與統計重設時機。
- [ ] 接續：`lib/raop_rtp_mirror.h/.c`、`renderers/video_renderer.h/.c` — 接收／解碼／呈現觀測點、pipeline 狀態與資源生命週期。
- [ ] 接續：`renderers/virtual_camera_writer.h/.c` — frame 格式、pipe 寫入限制、緩衝擁有權及停止行為。
- [ ] 持續：`uxplay_api.h` — 新增或變更 API 時同步維護宿主契約，不讓已補齊的註釋再次落後。

## 文件與驗收待辦

- [x] 建立獨立 Doxygen 設定、宿主 C API 說明與 changelogkit 設定。
- [ ] 逐步補齊 RTP、NTP、buffer 與 renderer 公開標頭的參數、回傳值與執行緒限制。
- [ ] 改善診斷計數器的跨執行緒同步，驗證資料競爭與快照一致性。
- [ ] 以實機 ingress／delivery 時間診斷確認音訊長時間落後的來源。
- [ ] 驗證持續音訊、網路抖動、連續缺包與重傳恢復的長時間穩定性。
- [ ] 驗證多輪斷線／重連後的執行緒、socket 與記憶體趨勢。
- [ ] 完成 AirPlay → virtual camera 的 iPad 端到端驗收。
