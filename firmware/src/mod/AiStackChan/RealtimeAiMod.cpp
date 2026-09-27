#if defined(REALTIME_API)

#include <Arduino.h>
#include <deque>
#include <SD.h>
#include <SPIFFS.h>
#include "mod/ModManager.h"
#include "RealtimeAiMod.h"
#include <Avatar.h>
#include "Robot.h"
#include "llm/ChatGPT/FunctionCall.h"
#include <WiFiClientSecure.h>
#include "Scheduler.h"
#include "MySchedule.h"
#include "share/SDUtil.h"

using namespace m5avatar;


/// 外部参照 ///
extern Avatar avatar;
extern bool servo_home;
extern volatile int servo_conv_state;   // 会話状態をサーボタスクへ伝える（0=待機 1=録音中 2=応答中）
extern void sw_tone();
extern void alarm_tone();
///////////////



RealtimeAiMod::RealtimeAiMod(bool _isOffline)
  : isOffline{_isOffline}
{
  // 録音トグルの当たり判定は元々「上端60px」だけで、画面中央を触っても反応しなかった。
  // 画面全体をタップ可能にする。
  // 右端40pxに置いていたサーボ切替とQR表示は廃止した。CoreS3ではサブウィンドウ自体が
  // 描画されない（Face.cpp が scale==1.0 のとき subWindow を描かない）ためQRは出ず、
  // 「押しても何も起きない罠」になっていたため。
  // box_t::contain() は w=0 のとき常に false を返すので setupBox(0,0,0,0) で無効化できる。
  const int16_t w = M5.Display.width();
  const int16_t h = M5.Display.height();
  box_stt.setupBox(0, 0, w, h);   // 画面全体が録音トグル
  box_servo.setupBox(0, 0, 0, 0); // 無効化
  box_BtnC.setupBox(0, 0, 0, 0);  // 無効化（QRはM5.BtnC経由では従来どおり呼べる）
  box_BtnA.setupBox(0, 0, 0, 0);  // 未使用

  lastTouchMs   = millis();
  recStartMs    = 0;
  prevRecording = false;
  prevUiState   = -1;

  pRtLLM = (RealtimeLLMBase*)robot->llm;
  pRtLLM->invokeWebSocketLoopTask();

  //servo_home = false;

#if 0
  if(!isOffline){
    //スケジューラ設定
    init_schedule();
  }
#endif
}


void RealtimeAiMod::init(void)
{
  //avatar.setSpeechText("Realtime AI");
  avatar.set_isSubWindowEnable(true);
  pRtLLM->resumeWebSocketLoopTask();
}

void RealtimeAiMod::pause(void)
{
  avatar.set_isSubWindowEnable(false);
  pRtLLM->suspendWebSocketLoopTask();
}


void RealtimeAiMod::update(int page_no)
{

}

void RealtimeAiMod::btnA_pressed(void)
{
#if defined(ARDUINO_M5STACK_ATOMS3R)
  Serial.println("Btn A pressed");
  sw_tone();
  toggleRealtimeRecord();
#endif
}

void RealtimeAiMod::btnB_longPressed(void)
{

}

void RealtimeAiMod::btnC_pressed(void)
{
  static bool isQrDrawing = false;
  if(!isQrDrawing){
    avatar.setSpeechText("");
    String url = String("http://") + WiFi.localIP().toString();
    avatar.updateSubWindowQrcode(url);
    avatar.set_isSubWindowEnable(true);
    isQrDrawing = true;
  }else{
    avatar.set_isSubWindowEnable(false);
    isQrDrawing = false;
  }
}

void RealtimeAiMod::display_touched(int16_t x, int16_t y)
{
  // 【重要】AI発話中のタップは無視する。
  // 発話中は webSocketLoopTask が音声ミューテックスを保持したままなので、ここで録音を
  // 開始してしまうと、同タスクが webSocketProcess() 内で自分の持つ非再帰ミューテックスを
  // 再取得しようとして永久にブロックし、WebSocket ごと停止する。
  // 状態は吹き出しの「おへんじ中」で伝わるため、無反応でも混乱しない。
  Serial.printf("[TOUCH] display_touched(%d,%d) speaking=%d ready=%d rec=%d\n",
                x, y, (int)pRtLLM->isSpeaking(), (int)pRtLLM->isReadyForRecord(),
                (int)pRtLLM->isRealtimeRecording());

  if (pRtLLM->isSpeaking()) { Serial.println("[TOUCH] rejected: speaking"); return; }

  lastTouchMs = millis();

  if (box_stt.contain(x, y))
  {
    // 未接続のままタップすると、録音は始まるのに送信は黙って捨てられ、
    // 来場者が最大30秒間「死んだ回線」に話し続けることになるので弾く。
    if (!pRtLLM->isReadyForRecord())
    {
      Serial.println("[TOUCH] rejected: not connected");
      return;
    }
    sw_tone();
    toggleRealtimeRecord();
  }
#ifdef USE_SERVO
  if (box_servo.contain(x, y))
  {
    sw_tone();
    servo_home = !servo_home;
  }
#endif
  if (box_BtnA.contain(x, y))
  {
    //sw_tone();
  }
  if (box_BtnC.contain(x, y))
  {
    btnC_pressed();
  }

}

void RealtimeAiMod::doubleTapped(float ax, float ay, float az)
{
  Serial.printf("Mod double tapped. ax=%.3f ay=%.3f az=%.3f\n", ax, ay, az);
#if defined(ARDUINO_M5STACK_ATOMS3R)
  sw_tone();
  toggleRealtimeRecord();
#endif
}


void RealtimeAiMod::idle(void)
{
#ifdef REALTIME_API_WITH_TTS

  if(robot->asyncPlaying || (pRtLLM->getOutputTextQueueSize() != 0)){
    // 発話中
    pRtLLM->setSpeaking(true);
    servo_home = false;
    avatar.setExpression(Expression::Happy);
  }
  else{
    // 発話停止中かつキューにテキストがない場合はLLM機能に発話終了を通知
    pRtLLM->setSpeaking(false);
    servo_home = true;
    avatar.setExpression(Expression::Neutral);
  }

#endif  //REALTIME_API_WITH_TTS

  // ここから下は REALTIME_API_WITH_TTS を使わない通常の Realtime(Gemini Live) 構成用。
  // 上のブロックは REALTIME_API_WITH_TTS 未定義のため丸ごとデッドコードになっており、
  // 結果として表情も首振りも一度も更新されていなかった（＝一番わかりやすい非言語の
  // フィードバックが眠っていた）ので、状態に応じて動かす。
#ifndef REALTIME_API_WITH_TTS
  {
    // 録音の立ち上がりを拾う。ターン終了時にファーム側が自動で録音を再開するため、
    // タップ経由以外でも録音開始時刻を更新しておかないと最短録音時間の判定が狂う。
    bool rec = pRtLLM->isRealtimeRecording();
    if(rec && !prevRecording){ recStartMs = millis(); }
    prevRecording = rec;

    // 0=待機 1=録音中 2=AI応答中
    int uiState = 0;
    if(pRtLLM->isSpeaking())                 uiState = 2;
    else if(pRtLLM->isRealtimeRecording())   uiState = 1;

    // 首の動きを会話に連動させる（サーボタスクが毎周期この値を見る）
    servo_conv_state = uiState;

    // LLM側も set_avatar_expression ツールで表情を変えるため、毎ループ上書きすると
    // 会話中の表情が潰れてしまう。状態が変わった瞬間だけ設定する。
    if(uiState != prevUiState){
      switch(uiState){
        case 1:   // 録音中：こちらを向いて聞く姿勢
          avatar.setExpression(Expression::Neutral);
          servo_home = false;
          break;
        case 2:   // AI応答中：うれしそうに動く
          avatar.setExpression(Expression::Happy);
          servo_home = false;
          break;
        default:  // 待機：正面に戻る
          avatar.setExpression(Expression::Neutral);
          servo_home = true;
          break;
      }
      prevUiState = uiState;
    }
  }
#endif

  // Alarm (Function Calling)
  alarmEventHandler();

#if 0 
  //スケジューラ処理
  if(!isOffline){
    run_schedule();
  }
#endif

}

void RealtimeAiMod::alarmEventHandler()
{
  if(xAlarmTimer != NULL){
    TickType_t xRemainingTime;

    /* Query the period of the timer that expires. */
    xRemainingTime = xTimerGetExpiryTime( xAlarmTimer ) - xTaskGetTickCount();
    avatarText = "Alarm countdown: " + String(xRemainingTime / 1000);
    avatar.set_isSubWindowEnable(true);
    avatar.updateSubWindowTxt(avatarText, 0, 0, 200, 50);
  }

  if (alarmTimerCallbacked) {
    alarmTimerCallbacked = false;
    avatar.set_isSubWindowEnable(false);
    alarm_tone();
  }

  if (alarmTimerCanceled) {
    alarmTimerCanceled = false;
    avatar.set_isSubWindowEnable(false);
  }

}

bool RealtimeAiMod::isBusy(void)
{
  if(pRtLLM->isRealtimeRecording() || pRtLLM->isSpeaking()){
    return true;
  }else{
    return false;
  }
}

void RealtimeAiMod::toggleRealtimeRecord(void)
{
  // 【タップ＝「いま話しかける」。タップで停止はしない】
  // 以前は ON/OFF のトグルだったため、反応が分からず連打した来場者が偶数回叩くと
  // 録音が切れて「触ったのに何も起きない・返事が来ない」状態になっていた（実機ログで確認）。
  // 録音の終了は Gemini 側の発話終了検知と REALTIME_RECORD_TIMEOUT に任せるほうが、
  // 初めて触る人には理解しやすい。
  static uint32_t lastToggleMs = 0;
  uint32_t now = millis();
  if(now - lastToggleMs < 800){ return; }   // 連打の取りこぼし防止
  lastToggleMs = now;

  if(pRtLLM->isRealtimeRecording()){
    // すでに録音中なら止めずに、聞いている時間を延長する（話し直しに対応）
    pRtLLM->resetRealtimeRecordStartTime();
    recStartMs = now;
  }else{
    recStartMs = now;
    pRtLLM->startRealtimeRecord();
  }
}

#endif //REALTIME_API