#if defined(REALTIME_API)

#ifndef _REALTIME_AI_MOD_H
#define _REALTIME_AI_MOD_H

#include <Arduino.h>
#include "mod/ModBase.h"
#include "llm/RealtimeLLMBase.h"

class RealtimeAiMod: public ModBase{
private:
    box_t box_servo;
    box_t box_stt;
    box_t box_BtnA;
    box_t box_BtnC;

    String avatarText;
    bool isOffline;

    uint32_t lastTouchMs;      // 最後に画面が触られた時刻
    uint32_t recStartMs;       // 録音が始まった時刻（最短録音時間の判定用）
    bool     prevRecording;    // 録音状態の立ち上がりエッジ検出用
    int      prevUiState;      // 表情・首振りを状態遷移時のみ更新するための前回状態

    RealtimeLLMBase* pRtLLM;

    // for TTS
    String ttsText;

    // for alarm (Function Calling)
    void alarmEventHandler();

public:
    RealtimeAiMod(bool _isOffline);

    void init(void);
    void pause(void);
    void update(int page_no);
    void btnA_pressed(void);
    void btnB_longPressed(void);
    void btnC_pressed(void);
    void display_touched(int16_t x, int16_t y);
    void doubleTapped(float ax, float ay, float az);   // 加速度センサによるダブルタップ検出のコールバック。platformio.iniで-DENABLE_TAP_DETECTを有効にしてください
    void idle(void);
    bool isBusy(void);

    void toggleRealtimeRecord(void);
};


#endif  //_REALTIME_AI_MOD_H

#endif  //REALTIME_API