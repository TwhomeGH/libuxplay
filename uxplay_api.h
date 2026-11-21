/**
 * @file uxplay_api.h
 * @brief 宿主 App 使用的程序全域 C 介面。
 * @details 同一程序一次只執行一個引擎。先註冊回呼，再在工作執行緒呼叫
 * start_uxplay()；stop_uxplay() 只發出要求，宿主仍須等待工作執行緒結束。
 * 回呼可能來自不同引擎執行緒，應立即複製需保留的字串並避免阻塞。
 * @warning 統計目前不是原子快照，亦未全面同步；不要據此推斷已驗證的
 * 無資料競爭保證。診斷計數不是播放完成通知。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 媒體格式與裝置資訊通知。
 * @param key 暫借的 UTF-8 欄位名稱。
 * @param value 暫借的 UTF-8 欄位內容，需保留時立即複製。
 */
typedef void (*uxplay_media_info_callback_t)(const char *key, const char *value);
/**
 * @brief 引擎日誌回呼，宿主應採非阻塞方式收集。
 * @param level 引擎 logger 的數值嚴重度。
 * @param message 暫借的 UTF-8 日誌，不得在回呼返回後持有指標。
 */
typedef void (*uxplay_log_callback_t)(int level, const char *message);
/**
 * @brief 連線狀態通知。
 * @param event 事件名稱，例如 connected、disconnected、rejected。
 * @param name 裝置名稱；無資料時為空字串。
 * @param model 裝置型號；無資料時為空字串。
 * @param deviceid 裝置識別值；無資料時為空字串。
 * @param open_connections 目前引擎回報的開啟連線數。
 */
typedef void (*uxplay_client_callback_t)(const char *event,
                                         const char *name,
                                         const char *model,
                                         const char *deviceid,
                                         int open_connections);
/**
 * @brief 引擎就緒通知；不代表已串流或已實際出聲。
 */
typedef void (*uxplay_ready_callback_t)(void);

/**
 * @brief 設定媒體資訊回呼，應於引擎啟動前或停止後操作。
 * @param callback 回呼函式；NULL 代表解除註冊。
 */
void set_uxplay_media_info_callback(uxplay_media_info_callback_t callback);
/**
 * @brief 更新 renderer 的相容 HUD 狀態；目前主 App 使用獨立 HUD 視窗。
 * @param text UTF-8 文字；NULL 視為空字串，函式內會複製。
 * @param right 非零表示靠右。
 * @param bottom 非零表示靠下。
 * @param xpad 水平邊距，單位像素。
 * @param ypad 垂直邊距，單位像素。
 */
void set_uxplay_hud_text(const char *text, int right, int bottom,
                         int xpad, int ypad);
/**
 * @brief 讀取影片呈現觀測點的影格統計。
 * @param[out] frames 累計影格數，可為 NULL。
 * @param[out] age_ms 距最後觀測的毫秒數；負值表示未知，可為 NULL。
 * @return 目前固定回傳 0，不表示實際顯示已驗收。
 */
int get_uxplay_frame_stats(uint64_t *frames, double *age_ms);
/**
 * @brief 讀取送入影片 appsrc 的影格統計。
 * @param[out] frames 累計輸入影格數，可為 NULL。
 * @param[out] age_ms 距最後輸入的毫秒數；負值表示未知，可為 NULL。
 * @return 目前固定回傳 0。
 */
int get_uxplay_video_input_stats(uint64_t *frames, double *age_ms);
/**
 * @brief 讀取影片解碼器輸出 probe 的統計。
 * @param[out] frames 累計解碼輸出數，可為 NULL。
 * @param[out] age_ms 距最後解碼輸出的毫秒數；負值表示未知，可為 NULL。
 * @return 目前固定回傳 0。
 */
int get_uxplay_video_decode_stats(uint64_t *frames, double *age_ms);
/**
 * @brief 讀取 parser 與 decoder 觀測點計數。
 * @param[out] parser_out parser 輸出數，可為 NULL。
 * @param[out] decoder_in decoder 輸入數，可為 NULL。
 * @param[out] decoder_out decoder 輸出數，可為 NULL。
 * @return 目前固定回傳 0；不同欄位不是一致快照。
 */
int get_uxplay_video_pipeline_stats(uint64_t *parser_out,
                                    uint64_t *decoder_in,
                                    uint64_t *decoder_out);
/**
 * @brief 讀取 parser 兩端的 probe 計數。
 * @param[out] parser_in parser 輸入數，可為 NULL。
 * @param[out] parser_out parser 輸出數，可為 NULL。
 * @return 目前固定回傳 0；未安裝 probe 時可能維持 0。
 */
int get_uxplay_video_parser_stats(uint64_t *parser_in,
                                   uint64_t *parser_out);
/**
 * @brief 取得目前記錄的影片解碼器名稱。
 * @return 引擎內部字串，不可釋放或修改；需保留時複製，無名稱時可為空字串。
 */
const char *get_uxplay_video_decoder(void);
/**
 * @brief 取得 libuxplay 提交與建置時間文字。
 * @return 程序內靜態字串，不可釋放；宿主宜在啟動階段先讀取。
 */
const char *get_uxplay_lib_build_info(void);
/**
 * @brief 讀取相容 overlay 兩端計數。
 * @param[out] in_frames overlay 輸入數，可為 NULL。
 * @param[out] out_frames overlay 輸出數，可為 NULL。
 * @return 目前固定回傳 0；無 overlay 時兩者可能維持 0。
 */
int get_uxplay_video_overlay_stats(uint64_t *in_frames,
                                   uint64_t *out_frames);
/**
 * @brief 讀取音訊接收與出列計數，不代表 AAC 解碼或實際出聲。
 * @param[out] received 接受進緩衝的封包數，包含明確無內容封包，可為 NULL。
 * @param[out] decoded 歷史命名；實際為已出列的音訊 payload 數，可為 NULL。
 * @param[out] resendRequests 補包要求次數，可為 NULL。
 * @param[out] emptyResends 無完整 RTP 音訊標頭的重傳回覆次數，可為 NULL。
 * @param[out] bytes 被接受封包的位元組累計，可為 NULL。
 * @return 目前固定回傳 0；欄位不是一致快照。
 */
int get_uxplay_audio_stats(uint64_t *received, uint64_t *decoded,
                           uint64_t *resendRequests, uint64_t *emptyResends,
                           uint64_t *bytes);
/**
 * @brief 讀取實際跳過的缺失序號數。
 * @param[out] recoveries 跳過數，明確無內容封包不算缺包，可為 NULL。
 * @return 目前固定回傳 0。
 */
int get_uxplay_audio_recovery_count(uint64_t *recoveries);
/**
 * @brief 讀取音訊管線恢復機制的重啟計數。
 * @param[out] restarts 計數，可為 NULL；目前缺包處理不主動重啟管線。
 * @return 目前固定回傳 0；不是所有狀態切換的總數。
 */
int get_uxplay_audio_pipeline_restarts(uint64_t *restarts);
/**
 * @brief 讀取最後跳過缺口時的等待時間。
 * @param[out] gap_ms 已收到後續封包的等待毫秒數，可為 NULL。
 * @return 目前固定回傳 0；初值 0 也可能表示尚無跳包事件。
 */
int get_uxplay_audio_last_gap_ms(uint64_t *gap_ms);
/**
 * @brief 讀取 level 訊息與 appsrc 接受計數，不是喇叭完成通知。
 * @param[out] peak level 的 dBFS 峰值；未觀測時初值 -1，可為 NULL。
 * @param[out] age_ms 距最後 level 訊息的毫秒數；UINT64_MAX 表示未知，可為 NULL。
 * @param[out] pushed appsrc 接受的 payload 數，flush 不清零，可為 NULL。
 * @return 目前固定回傳 0。
 */
int get_uxplay_audio_output_stats(double *peak, uint64_t *age_ms, uint64_t *pushed);
/**
 * @brief 讀取距最後音訊 payload 出列的時間。
 * @param[out] age_ms 毫秒數；UINT64_MAX 表示尚無出列資料，可為 NULL。
 * @return 目前固定回傳 0；不是網路延遲。
 */
int get_uxplay_audio_age_ms(uint64_t *age_ms);
/**
 * @brief 設定日誌回呼；有回呼時由宿主統一處理輸出。
 * @param callback 回呼函式；NULL 恢復引擎的主控台輸出。
 */
void set_uxplay_log_callback(uxplay_log_callback_t callback);
/**
 * @brief 設定連線事件回呼，應於引擎停止時操作。
 * @param callback 回呼函式；NULL 代表解除註冊。
 */
void set_uxplay_client_callback(uxplay_client_callback_t callback);
/**
 * @brief 設定引擎初始化通知，應於引擎停止時操作。
 * @param callback 回呼函式；NULL 代表解除註冊。
 */
void set_uxplay_ready_callback(uxplay_ready_callback_t callback);
/**
 * @brief 阻塞執行引擎，必須由宿主工作執行緒呼叫。
 * @param argc argv 的元素數，包含程式名稱。
 * @param argv 可寫的 UTF-8 命令列字串陣列；呼叫期間維持有效。
 * @return 正常停止或啟動被取消時為 0；初始化失敗等錯誤為非零。
 */
int start_uxplay(int argc, char *argv[]);
/**
 * @brief 發出停止要求並喚醒主迴圈；不等待資源回收。
 * @note 宿主需等待 start_uxplay() 返回後，才可解除回呼及釋放宿主物件。
 */
void stop_uxplay();
/**
 * @brief 重設部分全域控制旗標；start_uxplay() 自動呼叫。
 * @warning 不是清理函式，不可用於正在串流的引擎；也不會重設所有選項與統計。
 */
void reset_uxplay_state();

#ifdef __cplusplus
}
#endif
