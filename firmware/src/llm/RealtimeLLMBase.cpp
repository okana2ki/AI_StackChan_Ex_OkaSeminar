#if defined(REALTIME_API)

#include <Arduino.h>
#include <M5Unified.h>
#include <Avatar.h>
#include "share/Mutex.h"
//#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "rootCA/rootCAgoogleGemini.h"
#include <ArduinoJson.h>
#include "SpiRamJsonDocument.h"
#include "RealtimeLLMBase.h"
//#include "FunctionCall.h"
//#include "MCPClient.h"
#include "Robot.h"

#include <base64.h>
#include "libb64/cdecode.h"
#include <WebSocketsClient.h>

using namespace m5avatar;
extern Avatar avatar;

int16_t rtRecBuf[RT_REC_LENGTH];    // リアルタイム録音用メモリ
                                    // Core2だとヒープが不足するので静的な配列とした

TaskHandle_t webSocketLoopTask_h = NULL;

// WebSocketのイベント処理(webSocket.loop())及び、録音データ（約0.1秒）を
// WebSocketで送信するためのループタスク
void webSocketLoopTask(void *arg) {
    Serial.println("WebSocket loop task created");
    RealtimeLLMBase* pThis = (RealtimeLLMBase*)arg;

    while(1){
        pThis->webSocketProcess();
        //delay(1);     //webSocketProcess()内で状態によってスリープ時間を変更
    }
}


RealtimeLLMBase::RealtimeLLMBase(llm_param_t param) : 
    LLMBase(param, 0),
    msgDoc(0),
    rtRecSamplerate(RT_REC_SAMPLE_RATE),
    rtRecLength(RT_REC_LENGTH),
    realtime_recording(false),
    response_done(false),
    startTime(0),
    micFailCount(0),
    nextBufIdx(0),
    outputText(String(""))
{
#ifdef REALTIME_API_RECORD_TEST
  // リアルタイム録音のチャンクデータを蓄積してテスト再生するためのバッファ（約4s）
  recTestLenMax = rtRecLength * 40;
  recTestLenCnt = 0;
  recTestBuf = (int16_t*)heap_caps_malloc(recTestLenMax * sizeof(*rtRecBuf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif

#ifndef REALTIME_API_WITH_TTS
  // ストリーミング音声再生用のバッファを初期化（3枚。理由はヘッダのコメント参照）
  for(int i=0; i<AUDIO_BUF_NUM; i++){
    audioBuf[i] = (uint8_t*)malloc(100 * 1024);
    memset(audioBuf[i], 0, 100 * 1024);
  }
#endif

}

void RealtimeLLMBase::webSocketProcess()
{
    webSocket.loop();

#ifdef REALTIME_API_WITH_TTS
    if(response_done && !speaking){
        startRealtimeRecord();
        response_done = false;
    }
#endif

    if(realtime_recording){
        enterMutexAudio();
        //M5.Mic.begin();
        bool recOk = M5.Mic.record(rtRecBuf, rtRecLength, rtRecSamplerate);
        if(!recOk){
            // 録音失敗時の復旧。以前は Serial に出して delay(1000) するだけで、
            // (a) そのdelayがミューテックス保持中に入るためUI側のsw_tone()が1秒固まる
            // (b) 失敗しても古いバッファをそのまま送信し続ける
            // という2つの問題があった。ここで打ち切り、I2Sを再初期化する。
            micFailCount++;
            Serial.printf("Mic.record() returns false (%d)\n", micFailCount);
            if(micFailCount == 5){
                M5.Mic.end();
                delay(50);
                M5.Mic.begin();     // スピーカーとのI2S競合から復帰させる
            }
        }
        //M5.Mic.end();
        exitMutexAudio();

        if(!recOk){
            if(micFailCount >= 20){     // 約2秒粘ってもダメなら諦めて待機に戻す
                stopRealtimeRecord();
                micFailCount = 0;
                avatar.setSpeechText("マイク不調");
            }
            delay(100);                 // ミューテックスの外で待つ
            return;                     // 古いバッファは送らない
        }
        micFailCount = 0;

        String audio_base64;
        audio_base64 = base64::encode((u8*)rtRecBuf, rtRecLength * sizeof(int16_t));

#ifdef REALTIME_API_RECORD_TEST
        if((recTestLenCnt + rtRecLength) < recTestLenMax){
            memcpy((u8*)&recTestBuf[recTestLenCnt], (u8*)rtRecBuf, rtRecLength * sizeof(int16_t));
            recTestLenCnt += rtRecLength;
        }
#else
        String audioJsonBuf("");
        webSocket.sendTXT(buildInputAudioJson(audioJsonBuf, audio_base64));
#endif

        portTickType elapsedTime = checkRealtimeRecordTimeout();

        // 吹き出しは efontJA_16 を TEXT_SIZE=2 で描くため全角1文字=32px。
        // 全角8文字を超えると画面外にはみ出して切れるので、文言は必ず8文字以内にすること。
        // また Avatar::setSpeechText() は文字列をコピーせずポインタを保持する実装なので、
        // 渡してよいのは文字列リテラルか static バッファだけ（String::c_str() は不可）。
        if(elapsedTime > (REALTIME_RECORD_TIMEOUT - 4000)){
            avatar.setSpeechText("もうすぐ終わり");    // 打ち切りが近いことを予告する
        }else{
            avatar.setSpeechText("きいてるよ");
        }
        delay(1);
    }
    else{
        if(speaking){
            //発話中もしくはテキスト生成中
            // 以前は空文字で、AIが喋っている間だけ画面から文字が消えて
            // 「固まった？」と誤解される状態だったため文言を出す。
            avatar.setSpeechText("おへんじ中");
            resetRealtimeRecordStartTime(); //長いテキストを発話中にタイムアウトしてしまうのを防ぐ
            delay(1);
        }
        else{
            // 待機中は接続状態を出し分ける。
            // 以前は状態に関係なく "Please touch" を出していたため、WiFi断・APIキー失効・
            // サーバ切断のいずれでも画面は正常時と同じで、故障に気づけなかった。
            if(WiFi.status() != WL_CONNECTED){
                avatar.setSpeechText("ネットが切断");
            }else if(!webSocket.isConnected()){
                avatar.setSpeechText("じゅんび中");
            }else{
                avatar.setSpeechText("タッチしてね");
            }
            delay(10);
        }
    }
}

int RealtimeLLMBase::getAudioLevel()
{
    // 直近に再生へ回した面を見る（バッファが3枚になったので ^1 では正しく求まらない）
    int lastIdx = (nextBufIdx + AUDIO_BUF_NUM - 1) % AUDIO_BUF_NUM;
    return abs(*audioBuf[lastIdx]) * 50;
}

void RealtimeLLMBase::startRealtimeRecord()
{
    if(!realtime_recording){
        // [T] は応答速度の実測用。録音開始→AI音声開始→応答終了 の各時刻を残す。
        Serial.printf("[T %lu] Start realtime recording\n", (unsigned long)millis());
        realtime_recording = true;
        startTime = xTaskGetTickCount();
    }
}

void RealtimeLLMBase::stopRealtimeRecord()
{
    if(realtime_recording){
        // ここは「AIの最初の音声が届いた」タイミングでも呼ばれる（GeminiLive の初回 inlineData）。
        Serial.printf("[T %lu] Stop realtime recording\n", (unsigned long)millis());
        realtime_recording = false;
        startTime = 0;
    }
}

void RealtimeLLMBase::resetRealtimeRecordStartTime()
{
    startTime = xTaskGetTickCount();
}

portTickType RealtimeLLMBase::checkRealtimeRecordTimeout()
{
    portTickType elapsedTime;
    elapsedTime = (xTaskGetTickCount() - startTime) * portTICK_RATE_MS;
    if(elapsedTime > REALTIME_RECORD_TIMEOUT){
        Serial.println("Realtime recording timeout");
        stopRealtimeRecord();
#ifdef REALTIME_API_RECORD_TEST
        M5.Mic.end();
        if (M5.Speaker.begin())
        {
            M5.Speaker.playRaw(recTestBuf, recTestLenCnt, rtRecSamplerate);
            while (M5.Speaker.isPlaying()) { delay(10); }
            M5.Speaker.end();
            M5.Mic.begin();
        }
        recTestLenCnt = 0;
#endif
    }

    return elapsedTime;
}

int RealtimeLLMBase::base64_decode(const char* input, int size, char* output)
{
	/* keep track of our decoded position */
	char* c = output;
	/* store the number of bytes decoded by a single call */
	int cnt = 0;
	/* we need a decoder state */
	base64_decodestate s;
	
	/*---------- START DECODING ----------*/
	/* initialise the decoder state */
	base64_init_decodestate(&s);
	/* decode the input data */
	cnt = base64_decode_block(input, strlen(input), c, &s);
	c += cnt;
	/* note: there is no base64_decode_blockend! */
	/*---------- STOP DECODING  ----------*/
	
	/* we want to print the decoded data, so null-terminate it: */
	*c = 0;
	
	return cnt;
}


void RealtimeLLMBase::hexdump(const void *mem, uint32_t len, uint8_t cols) {
	const uint8_t* src = (const uint8_t*) mem;
	Serial.printf("\n[HEXDUMP] Address: 0x%08X len: 0x%X (%d)", (ptrdiff_t)src, len, len);
	for(uint32_t i = 0; i < len; i++) {
		if(i % cols == 0) {
			Serial.printf("\n[0x%08X] 0x%08X: ", (ptrdiff_t)src, i);
		}
		Serial.printf("%02X ", *src);
		src++;
	}
	Serial.printf("\n");
}


// 1発話ぶんの再生統計を出す。turnComplete のタイミングで呼ぶこと。
void RealtimeLLMBase::reportAudioStats(void)
{
    if(audioChunkCount == 0){ return; }
    uint32_t audioMs = (uint32_t)((uint64_t)audioPcmSamples * 1000 / 24000);
    uint32_t wallMs  = millis() - audioFirstMs;
    Serial.printf("[AUDIO] chunks=%lu starve=%lu audio=%lums wall=%lums wait=%lums avgchunk=%lums\n",
                  (unsigned long)audioChunkCount, (unsigned long)audioStarveCount,
                  (unsigned long)audioMs, (unsigned long)wallMs,
                  (unsigned long)audioWaitMs,
                  (unsigned long)(audioMs / audioChunkCount));
    audioChunkCount = 0; audioStarveCount = 0; audioPcmSamples = 0; audioWaitMs = 0;
}

void RealtimeLLMBase::streamAudioDelta(String& delta)
{
    if(audioChunkCount == 0){ audioFirstMs = millis(); }
    // 【音が途切れる（ギザギザする）問題への対策】
    // ここは音声チャンクごとに毎回呼ばれる「再生の心臓部」なので、
    // 少しでも待たせると音が途切れる。
    //
    // (1) Serial出力を削除
    //     115200bps では1行出すのに数msかかり、その間 再生が滞る。
    //     デバッグで必要なときだけ下の #if を 1 にする。
    //
    // (2) バッファ3枚 + キュー投入で「隙間なく」繋ぐ
    //     playRaw() はバッファをコピーせずポインタのまま再生するため、
    //     再生中/キュー待ちの面を上書きすると音が飛ぶ（速く聞こえる）。
    //     かつて2枚で「鳴り終わるまで待つ」方式にしていたが、それだと
    //     チャンクの間に必ず無音の隙間ができて途切れて聞こえた。
    //
    //     M5.Speaker.isPlaying(0) は 0=停止 / 1=再生中 / 2=再生中かつ次が待機中 を返す。
    //     「2 の間だけ待つ」＝常に1つ先を仕込んでおく状態を保てる。
    //     このとき使用中の面は最大2枚なので、3枚あれば書き込み先が必ず空いている。
    int base64Size = delta.length();
    uint8_t* buf = audioBuf[nextBufIdx];
    int len = base64_decode(delta.c_str(), base64Size, (char*)buf);

#if 0   // デバッグ用（音が途切れるので常用しないこと）
    Serial.printf("audio base64:%d pcm16:%d byte\n", base64Size, len);
#endif

    // (3) 【最重要】チャンネルを 0 に固定する
    //     playRaw(data, len, rate, stereo, repeat, channel, stop_current)
    //     channel の既定値は -1 ＝「空いている仮想チャンネルに自動割り当て」。
    //     既定のままだと音声チャンクが毎回ちがうチャンネルに載り、
    //     複数チャンクが同時に鳴ってしまう → 速く聞こえる・ギザギザする。
    //     さらに上の待ちは isPlaying(0) を見ているのに、データは別チャンネルへ
    //     行っていたので順番待ちが一切機能していなかった。
    //     ch0 に固定して初めて「1本の列に並べて順番に鳴らす」動作になる。
    // 【計測】ここに来た時点で ch0 が「停止(0)」なら、鳴らす音が尽きていた＝途切れた証拠。
    //         1発話ぶんまとめて turnComplete のときに出す（毎回出すと出力自体が音を止める）。
    if(M5.Speaker.isPlaying(0) == 0 && audioChunkCount > 0){ audioStarveCount++; }
    audioChunkCount++;
    audioPcmSamples += (len/2);
    uint32_t t0 = millis();

    while (M5.Speaker.isPlaying(0) > 1) { vTaskDelay(1); }
    audioWaitMs += (millis() - t0);
    M5.Speaker.playRaw((int16_t*)buf, len/2, 24000, false, 1, 0);
    if(++nextBufIdx >= AUDIO_BUF_NUM){ nextBufIdx = 0; }
}

void RealtimeLLMBase::invokeWebSocketLoopTask(void)
{
    xTaskCreate(webSocketLoopTask, /* Function to implement the task */
            "webSocketLoopTask", /* Name of the task */
            6*1024,               /* Stack size in words */
            this,                 /* Task input parameter */
            3,                    /* Priority of the task */
            &webSocketLoopTask_h);                /* Task handle. */
}

void RealtimeLLMBase::suspendWebSocketLoopTask(void)
{
    if (eTaskGetState(webSocketLoopTask_h) != eSuspended) {
      Serial.println("webSocketLoopTask Suspend");
      vTaskSuspend(webSocketLoopTask_h);
    }
}

void RealtimeLLMBase::resumeWebSocketLoopTask(void)
{
    if (eTaskGetState(webSocketLoopTask_h) == eSuspended) {
      Serial.println("webSocketLoopTask Resume");
      vTaskResume(webSocketLoopTask_h);
    }
}

#endif  //REALTIME_API