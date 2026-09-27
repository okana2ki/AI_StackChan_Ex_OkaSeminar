#include <Arduino.h>
//#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>
#include "share/Version.h"
#include "share/Mutex.h"
#include "share/SDUtil.h"
#include "share/DefaultParams.h"
#include <M5Unified.h>
#include <nvs.h>
#include <Avatar.h>
#include <faces/CatFace.h>
#include "StackchanExConfig.h" 
#include "Robot.h"
#include "mod/ModManager.h"
#include "mod/ModBase.h"
#include "mod/AiStackChan/AiStackChanMod.h"
#include "mod/AiStackChan/RealtimeAiMod.h"
#include "mod/Pomodoro/PomodoroMod.h"
#include "mod/PhotoFrame/PhotoFrameMod.h"
#include "mod/StatusMonitor/StatusMonitorMod.h"
#include "mod/VolumeSetting/VolumeSettingMod.h"
#include "mod/QRdisplay/QRdisplayMod.h"

#include "driver/PlayMP3.h"   //lipSync
#include "driver/TapDetect.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "SpiRamJsonDocument.h"
#include <ESP8266FtpServer.h>

#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/FunctionCall.h"
#include "llm/ChatHistory.h"
#include "llm/Gemini/GeminiLive.h"

#include "WebAPI.h"

#if defined( ENABLE_CAMERA )
#include "driver/Camera.h"
#endif    //ENABLE_CAMERA

#include "driver/WatchDog.h"
#include "SDUpdater.h"
#include "DebugTools.h"

#if defined(USE_AUDIO_MODULE)
#include "driver/M5AudioModule.h"
#endif

StackchanExConfig system_config;
Robot* robot;
bool isOffline = false;
AiStackChanMod* g_ai_stackchan_mod = nullptr;  // WebAPI から sleep 操作に使用


// NTP接続情報　NTP connection information.
const char* NTPSRV      = "ntp.jst.mfeed.ad.jp";    // NTPサーバーアドレス NTP server address.
const long  GMT_OFFSET  = 9 * 3600;                 // GMT-TOKYO(時差９時間）9 hours time difference.
const int   DAYLIGHT_OFFSET = 0;                    // サマータイム設定なし No daylight saving time setting

//bool servo_home = false;
bool servo_home = true;
bool servo_manual = false;  // true のとき HeadMotionController がサーボを直接制御（サーボタスクはスキップ）

// 会話の状態をサーボタスクへ伝えるための共有変数。RealtimeAiMod::idle() が更新する。
// サーボタスクは元々 5 秒周期でランダムな視線方向へ動くだけで会話を一切見ていなかったため、
// 首の動きが会話と連動していなかった。
// 0=待機  1=録音中(聞いている)  2=AI応答中(話している)
volatile int servo_conv_state = 0;

using namespace m5avatar;
Avatar avatar;
Face* customFace;
const Expression expressions_table[] = {
  Expression::Neutral,
  Expression::Happy,
  Expression::Sleepy,
  Expression::Doubt,
  Expression::Sad,
  Expression::Angry
};

FtpServer ftpSrv;   //set #define FTP_DEBUG in ESP8266FtpServer.h to see ftp verbose on serial


void lipSync(void *args)
{
  int level = 0;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
#ifdef REALTIME_API
#ifdef REALTIME_API_WITH_TTS
    level = robot->tts->getLevel();
#else
    level = ((RealtimeLLMBase*)(robot->llm))->getAudioLevel();
#endif
#else
    level = robot->tts->getLevel();
#endif
    if(level<100) level = 0;
    if(level > 15000)
    {
      level = 15000;
    }
    float open = (float)level/15000.0;
    avatar->setMouthOpenRatio(open);
    delay(33);
  }
}


void servo(void *args)
{
  float gazeX, gazeY;
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  int  prevConv = -1;
  bool nodDown  = false;
  for (;;)
  {
#ifdef USE_SERVO
    // このタスクは専用スレッドなので moveXY() のブロックは他へ影響しない。
    const int conv = servo_conv_state;

    if(servo_manual)
    {
      // HeadMotionController が直接制御中 → タスクは何もしない
      prevConv = -1;
      delay(200);
    }
    else if(conv == 2)
    {
      // AI応答中：**サーボを動かさない**。
      // うなずき動作(moveXY)を入れていたが、moveXY は内部で待ちループを回すため
      // 音声のストリーミング再生とCPUを奪い合い、音が途切れる原因になっていた。
      // 音声品質を最優先し、発話中は静止させる。
      // （うなずきを戻すなら、音が途切れないことを実機で確認してから）
      if(prevConv != conv)
      {
        const int cx = system_config.getServoInfo(AXIS_X)->start_degree;
        const int cy = system_config.getServoInfo(AXIS_Y)->start_degree;
        robot->servo->moveXY(cx, constrain(cy - 8, 5, 85), 400);   // 話し始めに一度だけ顔を上げる
        prevConv = conv;
      }
      delay(200);
    }
    else if(conv == 1)
    {
      // 録音中：正面を向いて静かに待つ。
      // 聞いている間は動かさないほうが「聞いてくれている」感じになるので、
      // 状態が変わった最初の一回だけ正面へ向ける。
      if(prevConv != conv)
      {
        const int cx = system_config.getServoInfo(AXIS_X)->start_degree;
        const int cy = system_config.getServoInfo(AXIS_Y)->start_degree;
        robot->servo->moveXY(cx, constrain(cy - 4, 5, 85), 500);
        prevConv = conv;
      }
      delay(200);
    }
    else
    {
      // 待機中：従来どおり。ゆっくり周囲を見る or 正面に戻る。
      prevConv = conv;
      if(!servo_home)
      {
        avatar->getGaze(&gazeY, &gazeX);
        robot->servo->moveToGaze((int)(15.0 * gazeX), (int)(10.0 * gazeY));
      } else {
        robot->servo->moveToOrigin();
      }
      delay(5000);
    }
#else
    delay(5000);
#endif
  }
}

void battery_check(void *args) {
  DriveContext *ctx = (DriveContext *)args;
  Avatar *avatar = ctx->getAvatar();
  for (;;)
  {
    int32_t batteryLevel = M5.Power.getBatteryLevel();
    if((batteryLevel < 95) && (batteryLevel != 0)){
      avatar->setBatteryIcon(true);
      avatar->setBatteryStatus(M5.Power.isCharging(), M5.Power.getBatteryLevel());
    }
    else{
      avatar->setBatteryIcon(false);    
    }
    delay(60000);
  }
}

bool Wifi_connection_check() {
  unsigned long start_millis = millis();

  // 前回接続時情報で接続する
  while (WiFi.status() != WL_CONNECTED) {
    M5.Display.print(".");
    Serial.print(".");
    delay(1000);
    // 5秒以上接続できなかったら抜ける
    if ( 5000 < (millis() - start_millis) ) {
      //break;
      return false;
    }
  }
  return true;
}

bool WifiSmartConfig() {
#if defined(USE_LLM_MODULE)
  // LLMモジュール使用時は普通はオフラインが前提のため、Smart Config待ちはしない
  return false;
#else
  unsigned long start_millis = millis();
  WiFi.mode(WIFI_STA);
  WiFi.beginSmartConfig();
  M5.Display.println("Waiting for SmartConfig");
  Serial.println("Waiting for SmartConfig");
  while (!WiFi.smartConfigDone()) {
    delay(1000);
    M5.Display.print("#");
    Serial.print("#");
    // 30秒以上接続できなかったら抜ける
    if ( 30000 < millis() - start_millis) {
      Serial.println("");
      //Serial.println("Reset");
      //ESP.restart();
      return false;
    }
  }
  return true;
#endif
}

void time_sync(const char* ntpsrv, long gmt_offset, int daylight_offset) {
  struct tm timeInfo; 
  char buf[60];

  configTime(gmt_offset, daylight_offset, ntpsrv);          // NTPサーバと同期

  if (getLocalTime(&timeInfo)) {                            // timeinfoに現在時刻を格納
    Serial.print("NTP : ");                                 // シリアルモニターに表示
    Serial.println(ntpsrv);                                 // シリアルモニターに表示

    sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d\n",     // 表示内容の編集
    timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
    timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

    Serial.println(buf);                                    // シリアルモニターに表示
  }
  else {
    Serial.print("NTP Sync Error ");                        // シリアルモニターに表示
  }
}



ModBase* init_mod(void)
{
  ModBase* mod;
  if(!isOffline || robot->isAllOfflineService()){
#if defined(REALTIME_API)
    add_mod(new RealtimeAiMod(isOffline));
#else
    g_ai_stackchan_mod = new AiStackChanMod(isOffline);
    add_mod(g_ai_stackchan_mod);
#endif
  }
  add_mod(new StatusMonitorMod());
  add_mod(new VolumeSettingMod());
  //add_mod(new PomodoroMod(isOffline));
  //add_mod(new PhotoFrameMod(isOffline));
  //add_mod(new QRdisplayMod());
  mod = get_current_mod();
  mod->init();
  return mod;
}


void sw_tone()
{
  // 以前は enterMutexAudio() で無限待ちしていたため、AI発話中(webSocketLoopTaskが
  // ミューテックスを保持中)にタップすると loop() が発話終了まで停止し、本体が固まっていた。
  // 取れなければ音を諦める。タップ自体は受理されるので操作感は損なわれない。
  if (!tryEnterMutexAudio(200)) { return; }
  M5.Mic.end();
  M5.Speaker.begin();
#if defined(ARDUINO_M5STACK_ATOMS3R)
  delay(300);     // AtomS3Rはこのdelayがないと鳴らないときがある
#endif
  M5.Speaker.tone(1000, 100);
  // 固定の delay(500) をやめ、実際の再生完了を待つ（約100ms）。
  // タップから録音開始までの待ちが約800ms→約120msになり、第一声の取りこぼしが減る。
  while (M5.Speaker.isPlaying()) { delay(1); }

  M5.Speaker.end();
  M5.Mic.begin();
  exitMutexAudio();
}
  
void alarm_tone()
{
  enterMutexAudio();
  M5.Mic.end();
  M5.Speaker.begin();

  for(int i=0; i<5; i++){
    M5.Speaker.tone(1200, 50);
    delay(100);
    M5.Speaker.tone(1200, 50);
    delay(100);
    M5.Speaker.tone(1200, 50);
    delay(1000);  
  }

  M5.Speaker.end();
  M5.Mic.begin();
  exitMutexAudio();
}

void init_mic_spk()
{
#if defined(USE_AUDIO_MODULE)
  initAudioModule();
#endif

  {
    auto micConfig = M5.Mic.config();
    //micConfig.stereo = false;
    micConfig.sample_rate = 16000;
#if defined(USE_AUDIO_MODULE)
    micConfig.pin_data_in = SYS_I2S_DIN_PIN;
    micConfig.pin_bck = SYS_I2S_SCLK_PIN;
    micConfig.pin_mck = SYS_I2S_MCLK_PIN;
    micConfig.pin_ws = SYS_I2S_LRCK_PIN;
#endif
    M5.Mic.config(micConfig);
  }
  M5.Mic.begin();

  { /// custom setting
    auto spk_cfg = M5.Speaker.config();
    /// Increasing the sample_rate will improve the sound quality instead of increasing the CPU load.
    spk_cfg.sample_rate = 64000; // default:64000 (64kHz)  e.g. 48000 , 50000 , 80000 , 96000 , 100000 , 128000 , 144000 , 192000 , 200000
    spk_cfg.task_pinned_core = APP_CPU_NUM;

#if defined(USE_AUDIO_MODULE)
    spk_cfg.pin_data_out = SYS_I2S_DOUT_PIN;
    spk_cfg.pin_bck = SYS_I2S_SCLK_PIN;
    spk_cfg.pin_mck = SYS_I2S_MCLK_PIN;
    spk_cfg.pin_ws = SYS_I2S_LRCK_PIN;
#endif
    M5.Speaker.config(spk_cfg);
  }
  //M5.Speaker.begin();
}

void setup()
{
  auto cfg = M5.config();

#if defined(ARDUINO_M5STACK_ATOMS3R)
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.external_speaker.atomic_echo = true;
#endif
  cfg.serial_baudrate = 115200;   //M5Unified 0.1.17からデフォルトが0になったため設定
  M5.begin(cfg);

  M5.Display.setBrightness(255);  // 画面が暗すぎる対策（M5.begin既定値だと視認困難なため明示的に明るくする）
  Serial.printf("[DIAG] after M5.begin: board=%d brightness=%d\n", (int)M5.getBoard(), M5.Display.getBrightness());

  /// シリアル出力のログレベルを VERBOSEに設定
  //M5.Log.setLogLevel(m5::log_target_serial, ESP_LOG_VERBOSE);


#if defined(ARDUINO_M5STACK_ATOMS3R)
  M5.Lcd.setTextSize(2);
  M5.Lcd.printf("Ver.%s\n", FW_VERSION);
#else
  M5.Lcd.setFont(&fonts::lgfxJapanGothic_20);
  M5.Lcd.setTextSize(1);
  M5.Lcd.println("AIスタックチャン [・＿・]");
  M5.Lcd.printf("バージョン: %s\n", FW_VERSION);
  M5.Lcd.println("じゅんびしています...");
#endif

  initMutex();

#if defined(ENABLE_SD_UPDATER)
  // ***** for SD-Updater *********************
  SDU_lobby("AiStackChanEx");
  // ******************************************
#endif

  //auto brightness = M5.Display.getBrightness();
  //Serial.printf("Brightness: %d\n", brightness);

  init_mic_spk();

  /// settings
#if defined(ARDUINO_M5STACK_ATOMS3R)
  if (SPIFFS.begin()) {
    // この関数ですべてのYAMLファイル(Basic, Secret, Extend)を読み込む
    system_config.loadConfig(SPIFFS, "/SC_ExConfig.yaml", 2048,
                                     "/SC_SecConfig.yaml", 2048,
                                     "/SC_BasicConfig.yaml", 2048);
#else
  if (SD.begin(GPIO_NUM_4, SPI, 25000000)) {
    // この関数ですべてのYAMLファイル(Basic, Secret, Extend)を読み込む
    system_config.loadConfig(SD, "/app/AiStackChanEx/SC_ExConfig.yaml");
#endif
    // Wifi設定読み込み
    wifi_s* wifi_info = system_config.getWiFiSetting();
    Serial.printf("\nSSID: %s\n",wifi_info->ssid.c_str());
    Serial.printf("Key: %s\n",wifi_info->password.c_str());

    // 前回設定で接続
    Serial.println("Connecting to WiFi");
    WiFi.disconnect();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    if(Wifi_connection_check()){
      Serial.println("Successfully connected to Wi-Fi using the previous settings.");
    }else{
      // 前回設定での接続に失敗。SDカード設定による接続にトライ。
      Serial.println("The previous WiFi connection failed. Attempting to connect using the SD card settings.");
      if(wifi_info->ssid.length() == 0){
        // SDカード設定の取得に失敗。Smart Configをスタート。
        Serial.println("Can't get WiFi settings. Start Smart Config.");
        if(!WifiSmartConfig()){
          // Smart Config失敗。オフラインモード。
          Serial.println("Smart Config failed. Running in offline mode.");
          isOffline = true;
        }
      }else{
        WiFi.begin(wifi_info->ssid.c_str(), wifi_info->password.c_str());
        if(Wifi_connection_check()){
          // SDカード設定による接続に成功。
          Serial.println("Successfully established a Wi-Fi connection via the SD card settings.");
        }else{
          // SDカード設定による接続に失敗。Smart Configをスタート。
          Serial.println("WiFi connection failed due to SD card settings. Start Smart Config.");
          if(!WifiSmartConfig()){
            // Smart Config失敗。オフラインモード。
            Serial.println("Smart Config failed. Running in offline mode.");
            isOffline = true;
          }
        }
      }
    }

    if(!isOffline){
      Serial.println(WiFi.localIP());
      M5.Lcd.println(WiFi.localIP());
      delay(1000);

      //Webサーバ設定
      init_web_server();
      //FTPサーバ設定（SPIFFS用）
      ftpSrv.begin("stackchan","stackchan");    //username, password for ftp.  set ports in ESP8266FtpServer.h  (default 21, 50009 for PASV)
      Serial.println("FTP server started");
      M5.Lcd.println("サーバー起動OK");

      //時刻同期
      time_sync(NTPSRV, GMT_OFFSET, DAYLIGHT_OFFSET);
    }else{
      M5.Lcd.print("WiFiにつながりません\nオフラインで起動します\n");
    }

    robot = new Robot(system_config);

    //SD.end();
  } else {
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.print("SDカードを\nさしなおして\nください");
    delay(5000);
    ESP.restart();
    //WiFi.begin();
  }
  
  mp3_init();

  //mod設定
  init_mod();

  // 【重要】日本語フォントの設定は avatar.init() より前に行う。
  // init() は描画タスクを起動するため、後から設定すると起動直後の数フレームは
  // speechFont が nullptr のまま描画され、ASCII専用フォントにフォールバックして
  // 日本語が豆腐・文字化けになる。
  avatar.setSpeechFont(&fonts::efontJA_16);

#if defined(ARDUINO_M5STACK_ATOMS3R)
#if defined(CAT_FACE)
  customFace = new CatFace();
  avatar.setFace(customFace);
#endif
  avatar.setScale(0.5);
  avatar.setPosition(-56, -96);
  avatar.init();
#else
  //avatar.init();
  avatar.init(16);
#endif

  avatar.addTask(lipSync, "lipSync", 2048, 2);
  avatar.addTask(servo, "servo", 2048);
  avatar.addTask(battery_check, "battery_check", 2048);

  Serial.printf("Speaker volume (yaml): %d\n", system_config.getExConfig().audio.speaker_volume);
  if(0 != system_config.getExConfig().audio.speaker_volume){
    robot->spk_volume = system_config.getExConfig().audio.speaker_volume;
  }else{
    robot->spk_volume = DEFAULT_SPEAKER_VOLUME;
  }
  Serial.printf("Speaker volume (set): %d\n", robot->spk_volume);
  M5.Speaker.setVolume(robot->spk_volume);

#if defined(ENABLE_CAMERA)
  camera_init();
  avatar.set_isSubWindowEnable(true);
#endif

#if defined(ENABLE_TAP_DETECT)
  invokeDoubleTapDetectTask();
#endif

  //init_watchdog();

  //ヒープメモリ残量確認(デバッグ用)
  check_heap_free_size();
  check_heap_largest_free_block();

  // 画面が暗い問題の対策：M5.Display.setBrightness() が実バックライトに効かないため、
  // AXP2101 の BLDO1(LCDバックライト電源) を内部I2C経由で直接 ON・電圧3.3Vに設定する。
  // （CoreS3/M5Stackchan のバックライトは AXP2101 reg0x90 bit4=enable, reg0x96=voltage）
  M5.Display.setBrightness(255);
  M5.In_I2C.bitOn(0x34, 0x90, 0x10, 400000);          // AXP2101: BLDO1 enable
  M5.In_I2C.writeRegister8(0x34, 0x96, 0x1C, 400000); // AXP2101: BLDO1 = 3.3V (code 28)

  // タッチの判定しきい値。
  // 既定のフリック判定は 8px しかなく、指をわずかに動かしただけでフリック扱いになって
  // タップを取りこぼしていた。30px に上げてタップを拾いやすくする。
  // 長押しは 2 秒（右上長押しでmod切替する隠し操作用）。
  M5.Touch.setFlickThresh(30);
  M5.Touch.setHoldThresh(2000);

  // PY32 I/Oエキスパンダ(0x6F)の生存確認。
  // M5Stackchan のシリアルサーボは半二重で、送受信の切替をPY32が担うため、
  // PY32が応答しないと servo_type: "M5_SCS" が使えず首が動かせない。
  // 首が動かなくなったときに、PY32側かサーボ側かを切り分けるために残してある。
  // 正常時は ok=1 version=0x41 servo_type=4(M5_SCS)。ok=0 ならPY32が応答していない。
  {
    uint8_t py32ver = 0;
    bool py32ok = M5.In_I2C.readRegister(0x6F, 0x02, &py32ver, 1, 100000);
    Serial.printf("[DIAG] PY32(0x6F) probe: ok=%d version=0x%02X servo_type=%d\n",
                  (int)py32ok, py32ver, (int)system_config.getServoType());
  }
  Serial.printf("[DIAG] end of setup: board=%d brightness=%d, forced BLDO1 ON 3.3V\n",
                (int)M5.getBoard(), M5.Display.getBrightness());
}



void loop()
{
  // [ALIVE] メインループが回っているかの確認用。
  // アバター描画とWebSocketは別タスクなので、loop() だけが止まっていても
  // 画面は動いて見える。タッチが効かないときの切り分けに使う。
  {
    static uint32_t s_aliveMs = 0;
    static uint32_t s_loops = 0;
    s_loops++;
    // AI発話中は音声のストリーミング再生を最優先するためログを出さない。
    // （Serial出力は115200bpsで1行あたり数ms止まるため、音が途切れる一因になる）
    if (millis() - s_aliveMs > 5000 && servo_conv_state != 2) {
      s_aliveMs = millis();
      Serial.printf("[ALIVE] loop running (%lu laps) touchCount=%d\n",
                    (unsigned long)s_loops, (int)M5.Touch.getCount());
      s_loops = 0;
    }
  }

  //get_elapsed_time_micro("loop() start");
  M5.update();
  //get_elapsed_time_micro("M5.update time");
  ModBase* mod = get_current_mod();
  mod->idle();
  //get_elapsed_time_micro("Mod idle time");

  if (M5.BtnA.wasPressed())
  {
    mod->btnA_pressed();
  }

  if (M5.BtnA.pressedFor(2000))
  {
    mod->btnA_longPressed();
  }

  if (M5.BtnB.wasPressed())
  {
    mod->btnB_pressed();
  }

  if (M5.BtnB.pressedFor(2000))
  {
    mod->btnB_longPressed();
  }

  if (M5.BtnC.wasPressed())
  {
    mod->btnC_pressed();
  }

#if defined(ARDUINO_M5STACK_Core2) || defined( ARDUINO_M5STACK_CORES3 )
  // タッチ判定は「指を離した瞬間 + ほとんど動いていない」ときだけ成立させる。
  // 以前は wasPressed()(触れた瞬間) で display_touched を呼び、さらに wasFlicked()(離した瞬間) で
  // mod切替もしていたため、少しでも指がぶれると「録音が始まった上に別画面へ飛ぶ」二重発火が起きていた。
  // 来場者がフリックでデバッグ画面に迷い込む事故を防ぐため、フリックによるmod切替は廃止し、
  // 右上を長押しする隠し操作（スタッフ用）に移した。
  static bool s_gestureConsumed = false;   // 長押しでmod切替した直後の「離し」を無効化する
  if (M5.Touch.getCount())
  {
    auto t = M5.Touch.getDetail();

    // [TOUCH] タッチ検出の診断ログ。状態が変わったときだけ出す（毎ループ出すと溢れる）。
    {
      static int s_prevState = -1;
      if ((int)t.state != s_prevState) {
        s_prevState = (int)t.state;
        Serial.printf("[TOUCH] state=%d x=%d y=%d base=(%d,%d) dx=%d dy=%d rel=%d hold=%d\n",
                      (int)t.state, t.x, t.y, t.base_x, t.base_y,
                      t.distanceX(), t.distanceY(),
                      (int)t.wasReleased(), (int)t.wasHold());
      }
    }

    // 隠しコマンド: 右上60x60を長押し(2秒) でmod切替。来場者が偶然踏むことはない。
    // 発話中・録音中は音声用ミューテックスを握ったままタスクが止まり全体が固まるため切り替えない。
    if (t.wasHold() && t.base_x > 260 && t.base_y < 60)
    {
      s_gestureConsumed = true;
      if (!mod->isBusy()) { change_mod(); }
    }

    if (t.wasReleased())
    {
      if (s_gestureConsumed) {
        s_gestureConsumed = false;
      } else if (abs(t.distanceX()) < 40 && abs(t.distanceY()) < 40) {
        mod->display_touched(t.base_x, t.base_y);
      }
    }
  }
#endif

#if defined(ENABLE_TAP_DETECT)
  if(doubleTapDetected){
    Serial.println("loop(): Double tap detected");
    mod->doubleTapped(detectedAcc[0], detectedAcc[1], detectedAcc[2]);
    doubleTapDetected = false;
  }

  // Modで重い処理をしている場合はダブルタップ検出を停止する
  if(mod->isBusy()){
    stopDoubleTapDetectTask();
  }else{
    resumeDoubleTapDetectTask();
  }
#endif
  //get_elapsed_time_micro("Callback process time");

  if(!isOffline){
    web_server_handle_client();
    ftpSrv.handleFTP();
  }

  //get_elapsed_time_micro("Web event process time");
  
  //reset_watchdog();
}
