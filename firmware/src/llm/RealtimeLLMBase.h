#if defined(REALTIME_API)

#ifndef _REALTIME_LLM_BASE_H
#define _REALTIME_LLM_BASE_H

#include <Arduino.h>
#include <M5Unified.h>
#include "StackchanExConfig.h"
#include "SpiRamJsonDocument.h"
#include "ChatHistory.h"
#include "LLMBase.h"
#include <WebSocketsClient.h>

//#define REALTIME_API_RECORD_TEST

#define GEMINI_PROMPT_MAX_SIZE   (1024*50)

#define RT_REC_LENGTH       (2000)      //0.125s 
#define RT_REC_SAMPLE_RATE  (16000)

#ifdef REALTIME_API_RECORD_TEST
#define REALTIME_RECORD_TIMEOUT     (4 * 1000)      //ms  ※録音テスト再生用バッファのサイズに合わせる
#else
// 展示向けに30秒→8秒へ短縮。
// AI応答後は録音が自動再開するため、誰も話さないと毎回この時間だけ無音を送り続ける。
// 実測でこの「待ち」が体感の遅さに直結していた（12秒設定で毎ターン12.0秒フルに消費）。
// 短すぎると考えながら話す人を切ってしまうので 8 秒程度が妥当。
#define REALTIME_RECORD_TIMEOUT     (8 * 1000)      //ms
#endif

extern String InitBuffer;
extern const String json_ChatString;

class RealtimeLLMBase: public LLMBase{
//private:
public:   //本当はprivateにしたいところだがコールバック関数にthisポインタを渡して使うためにpublicとした
    WebSocketsClient webSocket;
    SpiRamJsonDocument msgDoc;

    // for record
    //
    //int16_t* rtRecBuf;
    int rtRecSamplerate;
    int rtRecLength;
    bool realtime_recording;
    bool response_done;
    portTickType startTime;
    int micFailCount;       // Mic.record() の連続失敗回数（I2Sがスピーカーと競合すると false が返る）

#ifdef REALTIME_API_RECORD_TEST
    int16_t* recTestBuf;
    int recTestLenMax;
    int recTestLenCnt;
#endif

    // for play
    //
    // Base64をデコードして得た音声データを格納するバッファ。
    // 【3枚必要】M5.Speaker.playRaw() は渡したバッファをコピーせずポインタのまま再生するため、
    // 再生中の面を上書きすると音が飛ぶ（速く聞こえる）。
    // M5Unified のヘッダにも「実行時生成データは3枚を順番に使うこと」と明記されている。
    // 2枚だと「鳴り終わるまで待つ」しかなく、チャンクの間に無音の隙間ができて途切れる。
    // 3枚あれば「1枚再生中・1枚キュー待ち・1枚書き込み中」が同時に成立し、隙間なく繋がる。
    static const int AUDIO_BUF_NUM = 3;
    uint8_t* audioBuf[AUDIO_BUF_NUM];
    int nextBufIdx;          // 次回データを書き込む面（0→1→2→0…）

    // 【計測用】1発話ぶんの再生統計。途切れ(underrun)が起きているかを数える。
    uint32_t audioChunkCount = 0;   // 受け取った音声チャンク数
    uint32_t audioStarveCount = 0;  // 次が来る前に鳴り止んでいた回数＝途切れた回数
    uint32_t audioPcmSamples = 0;   // 音声の総サンプル数（再生されるべき長さ）
    uint32_t audioWaitMs = 0;       // キューが空くのを待った合計時間
    uint32_t audioFirstMs = 0;      // 最初のチャンクが来た時刻

public:
    RealtimeLLMBase(llm_param_t param);

    virtual void chat(String text, const char *base64_buf = NULL) {};   //dummy
    virtual String& buildInputAudioJson(String& jsonBuf, String& base64) = 0;

    void invokeWebSocketLoopTask(void);
    void suspendWebSocketLoopTask(void);
    void resumeWebSocketLoopTask(void);
    void webSocketProcess();
    int getAudioLevel();
    void startRealtimeRecord();
    void stopRealtimeRecord();
    void resetRealtimeRecordStartTime();
    portTickType checkRealtimeRecordTimeout();
    bool isRealtimeRecording() {return realtime_recording;};

    // 録音を始めてよい状態か（WebSocketが張れているか）。
    // 未接続で録音を始めると sendTXT() が黙って false を返して捨てるだけなので、
    // 来場者が無反応な端末に話し続ける事故を防ぐためUI側から参照する。
    bool isReadyForRecord() {return webSocket.isConnected();};

    int base64_decode(const char* input, int size, char* output);
    void hexdump(const void *mem, uint32_t len, uint8_t cols = 16);
    void streamAudioDelta(String& delta);
    void reportAudioStats(void);

    // for TTS
    //
    String outputText;

};


#endif  //_REALTIME_LLM_BASE_H

#endif  //REALTIME_API