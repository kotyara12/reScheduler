#include <time.h>
#include "reScheduler.h"
#include "reEvents.h"
#include "rLog.h"
#include "reEsp32.h"
#include "reParams.h"
#include "project_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
#include "reSysInfo.h"
#endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
#if CONFIG_EVENT_LOOP_STATISTIC_ENABLED
#include "reEventsStat.h"
#endif // CONFIG_EVENT_LOOP_STATISTIC_ENABLED

static const char* logTAG = "SCHD";
static const char* schedulerTaskName = "scheduler";

TaskHandle_t _schedulerTask;

#if CONFIG_SCHEDULER_STATIC_ALLOCATION
StaticTask_t _schedulerTaskBuffer;
StackType_t _schedulerTaskStack[CONFIG_SCHEDULER_STACK_SIZE];
#endif // CONFIG_SCHEDULER_STATIC_ALLOCATION

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Silent mode -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_SILENT_MODE_ENABLE

static uint32_t tsSilentMode = CONFIG_SILENT_MODE_INTERVAL;
static bool stateSilentMode = false;
static const char* tagSM = "TIME";

void silentModeRegister()
{
  paramsRegisterCommonValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, 
    CONFIG_SILENT_MODE_TOPIC, CONFIG_SILENT_MODE_NAME,
    CONFIG_MQTT_PARAMS_QOS, (void*)&tsSilentMode);
}

void silentModeCheck(const struct tm timeinfo)
{
  if (tsSilentMode > 0) {
    uint16_t t1 = tsSilentMode / 10000;
    uint16_t t2 = tsSilentMode % 10000;
    int16_t  t0 = timeinfo.tm_hour * 100 + timeinfo.tm_min;
    bool newSilentMode = (t1 < t2) ? ((t0 >= t1) && (t0 < t2)) : !((t0 >= t2) && (t1 > t0));
    rlog_v(tagSM, "Silent mode check: t0=%.4d, t1=%.4d, t2=%.4d, old_mode=%d, new_mode=%d", t0, t1, t2, stateSilentMode, newSilentMode);
    // If the regime has changed
    if (stateSilentMode != newSilentMode) {
      stateSilentMode = newSilentMode;
      if (newSilentMode) {
        rlog_i(tagSM, "Silent mode activated");
        eventLoopPost(RE_TIME_EVENTS, RE_TIME_SILENT_MODE_ON, nullptr, 0, portMAX_DELAY);
      } else {
        eventLoopPost(RE_TIME_EVENTS, RE_TIME_SILENT_MODE_OFF, nullptr, 0, portMAX_DELAY);
        rlog_i(tagSM, "Silent mode disabled");
      };
    };
  };
}

void silentModeCheckExternal()
{
  rlog_d(tagSM, "Silent mode check forced");
  time_t nowT;
  struct tm nowS;
  nowT = time(nullptr);
  localtime_r(&nowT, &nowS);
  silentModeCheck(nowS);
}

bool isSilentMode()
{
  return stateSilentMode;
}

#endif // CONFIG_SILENT_MODE_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ Task exec ------------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void schedulerTaskExec(void* args)
{
  static time_t nowT;
  static struct tm nowS;
  static uint8_t minLast = 255;

  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
  static esp_timer_t timerSysInfo;
  timerSet(&timerSysInfo, CONFIG_MQTT_SYSINFO_INTERVAL);
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

  #if CONFIG_EVENT_LOOP_STATISTIC_ENABLED
  eventStatStart();
  #endif // CONFIG_EVENT_LOOP_STATISTIC_ENABLED

  while (true) {
    // Get the current time
    nowT = time(nullptr);
    localtime_r(&nowT, &nowS);
    if (nowS.tm_min != minLast) {
      minLast = nowS.tm_min;

      // Publish an event every minute
      eventLoopPost(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, nullptr, 0, portMAX_DELAY);

      // Calculate the operating time of the device
      sysinfoWorkTimeInc();

      if (nowT > 1000000000) {
        // Create strings with date and time
        sysinfoFixDateTime(nowS);

        // Post generated strings with date and time
        #if CONFIG_MQTT_TIME_ENABLE
        mqttPublishDateTime(nowS);
        #endif // CONFIG_MQTT_TIME_ENABLE

        // Check night (silent) mode
        #if CONFIG_SILENT_MODE_ENABLE
        silentModeCheck(nowS);
        #endif // CONFIG_SILENT_MODE_ENABLE
      };
    };

    // Publish system information
    #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE || CONFIG_EVENT_LOOP_STATISTIC_ENABLED
    if (timerTimeout(&timerSysInfo)) {
      timerSet(&timerSysInfo, CONFIG_MQTT_SYSINFO_INTERVAL);
      
      #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
      sysinfoPublishSysInfo();
      #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

      #if CONFIG_EVENT_LOOP_STATISTIC_ENABLED
      eventStatMqttPublish();
      #endif // CONFIG_EVENT_LOOP_STATISTIC_ENABLED
    };
    #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE || CONFIG_EVENT_LOOP_STATISTIC_ENABLED

    vTaskDelay(CONFIG_SCHEDULER_DELAY);
  };

  schedulerTaskDelete();
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Task routines ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool schedulerTaskCreate(bool createSuspended)
{
  if (!_schedulerTask) {
    // Create the scheduler task
    #if CONFIG_SCHEDULER_STATIC_ALLOCATION
    _schedulerTask = xTaskCreateStaticPinnedToCore(schedulerTaskExec, schedulerTaskName, CONFIG_SCHEDULER_STACK_SIZE, NULL, CONFIG_SCHEDULER_PRIORITY, _schedulerTaskStack, &_schedulerTaskBuffer, CONFIG_SCHEDULER_CORE); 
    #else
    xTaskCreatePinnedToCore(schedulerTaskExec, schedulerTaskName, CONFIG_SCHEDULER_STACK_SIZE, NULL, CONFIG_SCHEDULER_PRIORITY, &_schedulerTask, CONFIG_SCHEDULER_CORE); 
    #endif // CONFIG_SCHEDULER_STATIC_ALLOCATION
    if (!_schedulerTask) {
      rloga_e("Failed to create scheduler task!");
      return false;
    }
    else {
      #if CONFIG_SILENT_MODE_ENABLE
      silentModeRegister();
      #endif // CONFIG_SILENT_MODE_ENABLE
      if (createSuspended) {
        rloga_i("Task [ %s ] has been successfully created", schedulerTaskName);
        schedulerTaskSuspend();
        return schedulerEventHandlerRegister();
      } else {
        rloga_i("Task [ %s ] has been successfully started", schedulerTaskName);
        return true;
      };
    };
  };
  return false;
}

bool schedulerTaskSuspend()
{
  if ((_schedulerTask) && (eTaskGetState(_schedulerTask) != eSuspended)) {
    vTaskSuspend(_schedulerTask);
    if (eTaskGetState(_schedulerTask) == eSuspended) {
      rloga_d("Task [ %s ] has been suspended", schedulerTaskName);
      return true;
    } else {
      rloga_e("Failed to suspend task [ %s ]!", schedulerTaskName);
    };
  };
  return false;
}

bool schedulerTaskResume()
{
  if ((_schedulerTask) && (eTaskGetState(_schedulerTask) == eSuspended)) {
    vTaskResume(_schedulerTask);
    if (eTaskGetState(_schedulerTask) != eSuspended) {
      rloga_i("Task [ %s ] has been successfully resumed", schedulerTaskName);
      return true;
    } else {
      rloga_e("Failed to resume task [ %s ]!", schedulerTaskName);
    };
  };
  return false;
}


void schedulerTaskDelete()
{
  if (_schedulerTask) {
    schedulerEventHandlerUnregister();
    vTaskDelete(_schedulerTask);
    _schedulerTask = nullptr;
    rloga_d("Task [ %s ] was deleted", schedulerTaskName);
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------ Set time event handler -----------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void schedulerEventHandlerTime(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (_schedulerTask) {
    schedulerTaskResume();
  } else {
    schedulerTaskCreate(false);
  };
}

#if CONFIG_SILENT_MODE_ENABLE

static void schedulerEventHandlerSilentMode(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_PARAMS_CHANGED)  {
    if (*(uint32_t*)event_data == (uint32_t)&tsSilentMode) {
      silentModeCheckExternal();
    };
  };
}
#endif // CONFIG_SILENT_MODE_ENABLE

bool schedulerEventHandlerRegister()
{
  bool ret = eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime, nullptr)
          && eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime, nullptr);
  #if CONFIG_SILENT_MODE_ENABLE
    ret = ret && eventHandlerRegister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerSilentMode, nullptr);
  #endif // CONFIG_SILENT_MODE_ENABLE
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    ret = ret && sysinfoEventHandlerRegister();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
  return ret;
}

void schedulerEventHandlerUnregister()
{
  eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime);
  eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime);
  #if CONFIG_SILENT_MODE_ENABLE
    eventHandlerUnregister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerSilentMode);
  #endif // CONFIG_SILENT_MODE_ENABLE
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    sysinfoEventHandlerUnregister();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
}
