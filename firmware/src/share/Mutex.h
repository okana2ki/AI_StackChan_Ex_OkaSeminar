#ifndef _MUTEX_H
#define _MUTEX_H
#include <M5Unified.h>

extern SemaphoreHandle_t mutexAudio;

void initMutex(void);
void enterMutexAudio(void);
void exitMutexAudio(void);

// 時間制限つきで音声ミューテックスを取得する。取得できたら true。
// AI発話中は webSocketLoopTask がミューテックスを長時間保持するため、
// UI(loopタスク)側が enterMutexAudio() で無限待ちすると本体全体が固まる。
// UIから音を鳴らす用途ではこちらを使い、取れなければ諦めること。
bool tryEnterMutexAudio(uint32_t timeout_ms);

#endif