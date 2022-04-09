#include <time.h>
#include "reScheduler.h"
#include "reEvents.h"
#include "rLog.h"
#include "rTypes.h"
#include "reNvs.h"
#include "reEsp32.h"
#include "esp_timer.h"
#include "reParams.h"
#include "sys/queue.h"
#include "project_config.h"
#include "def_consts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
#include "reSysInfo.h"
#endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

static const char* logTAG = "SCHD";

typedef struct schedulerItem_t {
  timespan_t* timespan;
  int8_t state;
  uint_fast32_t value;
  STAILQ_ENTRY(schedulerItem_t) next;
} schedulerItem_t;
typedef struct schedulerItem_t *schedulerItemHandle_t;
STAILQ_HEAD(schedulerItemHead_t, schedulerItem_t);
typedef struct schedulerItemHead_t *schedulerItemHeadHandle_t;
static schedulerItemHeadHandle_t schedulerItems = nullptr;

static bool _handlersRegistered = false;
static esp_timer_handle_t _schedulerTimerMain = nullptr;
#if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
static esp_timer_handle_t _schedulerTimerSysInfo = nullptr;
#endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
#if CONFIG_MQTT_TASKLIST_ENABLE
static esp_timer_handle_t _schedulerTimerTasks = nullptr;
#endif // CONFIG_MQTT_TASKLIST_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------- Common functions ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool schedulerInit()
{
  if (!schedulerItems) {
    schedulerItems = (schedulerItemHeadHandle_t)esp_calloc(1, sizeof(schedulerItemHead_t));
    RE_MEM_CHECK(logTAG, schedulerItems, return false);
    STAILQ_INIT(schedulerItems);
  };
  return true;
}

void schedulerFree()
{
  if (schedulerItems) {
    schedulerItemHandle_t itemL, tmpL;
    STAILQ_FOREACH_SAFE(itemL, schedulerItems, next, tmpL) {
      STAILQ_REMOVE(schedulerItems, itemL, schedulerItem_t, next);
      free(itemL);
    };
    free(schedulerItems);
    schedulerItems = nullptr;
  };
}

bool schedulerRegister(timespan_t* timespan, uint32_t value)
{
  if (schedulerItems) {
    schedulerItemHandle_t item = (schedulerItemHandle_t)esp_calloc(1, sizeof(schedulerItem_t));
    RE_MEM_CHECK(logTAG, item, return false);
    item->timespan = timespan;
    item->value = value;
    item->state = -1;
    STAILQ_INSERT_TAIL(schedulerItems, item, next);
    return true;
  };
  return false;
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Silent mode -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE

static timespan_t tsSilentModeTimespan = CONFIG_SILENT_MODE_INTERVAL;
#if defined(CONFIG_SILENT_MODE_EXTENDED) && CONFIG_SILENT_MODE_EXTENDED
static bool enabledSilentMode = true;
#endif // CONFIG_SILENT_MODE_EXTENDED
static bool stateSilentMode = false;
static const char* tagSM = "TIME";

void silentModeRegister()
{
  #if defined(CONFIG_SILENT_MODE_EXTENDED) && CONFIG_SILENT_MODE_EXTENDED
    paramsGroupHandle_t _pgSilentMode = paramsRegisterGroup(nullptr, CONFIG_SILENT_MODE_PGROUP_KEY, CONFIG_SILENT_MODE_PGROUP_TOPIC, CONFIG_SILENT_MODE_PGROUP_FRIENDLY);
    if (_pgSilentMode) {
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_U8, nullptr, _pgSilentMode, 
        CONFIG_SILENT_MODE_ENABLE_TOPIC, CONFIG_SILENT_MODE_ENABLE_FRIENDLY,
        CONFIG_MQTT_PARAMS_QOS, (void*)&enabledSilentMode);
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, _pgSilentMode, 
        CONFIG_SILENT_MODE_TIMESPAN_TOPIC, CONFIG_SILENT_MODE_TIMESPAN_FRIENDLY,
        CONFIG_MQTT_PARAMS_QOS, (void*)&tsSilentModeTimespan);
    };
  #else
    paramsRegisterCommonValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, 
      CONFIG_SILENT_MODE_TOPIC, CONFIG_SILENT_MODE_FRIENDLY,
      CONFIG_MQTT_PARAMS_QOS, (void*)&tsSilentModeTimespan);
  #endif // CONFIG_SILENT_MODE_EXTENDED
}

void silentModeCheck(struct tm* timeinfo)
{
  #if defined(CONFIG_SILENT_MODE_EXTENDED) && CONFIG_SILENT_MODE_EXTENDED
  if (enabledSilentMode && (tsSilentModeTimespan > 0)) {
  #else
  if (tsSilentModeTimespan > 0) {
  #endif // CONFIG_SILENT_MODE_EXTENDED
    bool newSilentMode = checkTimespan(timeinfo, tsSilentModeTimespan);
    // If the mode has changed
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
  time_t nowT;
  struct tm nowS;
  nowT = time(nullptr);
  localtime_r(&nowT, &nowS);
  silentModeCheck(&nowS);
}

bool isSilentMode()
{
  #if defined(CONFIG_SILENT_MODE_EXTENDED) && CONFIG_SILENT_MODE_EXTENDED
    return enabledSilentMode && stateSilentMode;
  #else
    return stateSilentMode;
  #endif // CONFIG_SILENT_MODE_EXTENDED
}

#endif // CONFIG_SILENT_MODE_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------ Main Timer -----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void schedulerTimerMainExec(struct tm* nowS, bool isCorrectTime)
{
  // Publish an event every minute
  eventLoopPost(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &nowS->tm_min, sizeof(int), portMAX_DELAY);

  // Publish an event about beginning of next interval
  if (nowS->tm_min == 0) {
    eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_HOUR, &nowS->tm_hour, sizeof(int), portMAX_DELAY);
    if (nowS->tm_hour == 0) {
      eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_DAY, &nowS->tm_mday, sizeof(int), portMAX_DELAY);
      if (nowS->tm_wday == CONFIG_FORMAT_FIRST_DAY_OF_WEEK) {
        eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_WEEK, &nowS->tm_wday, sizeof(int), portMAX_DELAY);
      };
      if (nowS->tm_mday == 1) {
        eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_MONTH, &nowS->tm_mon, sizeof(int), portMAX_DELAY);
        if (nowS->tm_mon == 1) {
          eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_YEAR, &nowS->tm_year, sizeof(int), portMAX_DELAY);
        };
      };
    };
  };

  // Calculate the operating time of the device
  sysinfoWorkTimeInc();

  if (isCorrectTime) {
    // Create strings with date and time
    sysinfoFixDateTime(nowS);

    // Post generated strings with date and time
    #if CONFIG_MQTT_TIME_ENABLE
    mqttPublishDateTime(nowS);
    #endif // CONFIG_MQTT_TIME_ENABLE

    // Check schedule list
    if (schedulerItems) {
      schedulerItemHandle_t item;
      STAILQ_FOREACH(item, schedulerItems, next) {
        int8_t newState = checkTimespan(nowS, *item->timespan);
        if (newState != item->state) {
          item->state = newState;
          if (newState == 1) {
            eventLoopPost(RE_TIME_EVENTS, RE_TIME_TIMESPAN_ON, (void*)item->value, sizeof(item->value), portMAX_DELAY);
          } else {
            eventLoopPost(RE_TIME_EVENTS, RE_TIME_TIMESPAN_OFF, (void*)item->value, sizeof(item->value), portMAX_DELAY);
          };
        };
      };
    };

    // Check night (silent) mode
    #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
    silentModeCheck(nowS);
    #endif // CONFIG_SILENT_MODE_ENABLE
  };
}

static void schedulerTimerMainTimeout(void* arg)
{
  static struct timeval now_time;
  static struct tm now_tm;
  
  // Get current time
  gettimeofday(&now_time, nullptr);
  localtime_r(&now_time.tv_sec, &now_tm);
  
  // Process schedules
  schedulerTimerMainExec(&now_tm, now_time.tv_sec > 1000000000);

  // Calculate the timeout until the beginning of the next minute
  gettimeofday(&now_time, nullptr);
  localtime_r(&now_time.tv_sec, &now_tm);
  uint32_t timeout_us = ((int)60 - (int)now_tm.tm_sec) * 1000000 - now_time.tv_usec;
  RE_OK_CHECK_FIRST(logTAG, esp_timer_start_once(_schedulerTimerMain, timeout_us), return);
  rlog_d(logTAG, "Restart schedule timer for %d microseconds (sec=%d, usec=%d)", timeout_us, (int)now_tm.tm_sec, now_time.tv_usec);  
}

static bool schedulerTimerMainCreate()
{
  if (!_schedulerTimerMain) {
    esp_timer_create_args_t cfgTimer;
    memset(&cfgTimer, 0, sizeof(cfgTimer));
    cfgTimer.name = "scheduler_main";
    cfgTimer.callback = schedulerTimerMainTimeout;
    RE_OK_CHECK_FIRST(logTAG, esp_timer_create(&cfgTimer, &_schedulerTimerMain), return false);
    if (_schedulerTimerMain) {
      RE_OK_CHECK(logTAG, esp_timer_start_once(_schedulerTimerMain, 1000000), return false);
    };
    return true;
  };
  return false;
}

static void schedulerTimerMainDelete()
{
  if (_schedulerTimerMain) {
    if (esp_timer_is_active(_schedulerTimerMain)) {
      esp_timer_stop(_schedulerTimerMain);
    };
    RE_OK_CHECK_FIRST(logTAG, esp_timer_delete(_schedulerTimerMain), return);
    _schedulerTimerMain = nullptr;
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------- Status Timer ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

static void schedulerTimerSysInfoExec(void* arg)
{
  sysinfoPublishSysInfo();
}

static bool schedulerTimerSysInfoStart()
{
  if (_schedulerTimerSysInfo) {
    if (esp_timer_is_active(_schedulerTimerSysInfo)) {
      esp_timer_stop(_schedulerTimerSysInfo);
    };
    RE_OK_CHECK_FIRST(logTAG, esp_timer_start_periodic(_schedulerTimerSysInfo, CONFIG_MQTT_SYSINFO_INTERVAL * 1000), return false);
    rlog_i(logTAG, "Timer [scheduler_sysinfo] was started");
    return true;
  };
  return false;
}

static bool schedulerTimerSysInfoCreate(bool createSuspened)
{
  if (!_schedulerTimerSysInfo) {
    esp_timer_create_args_t cfgTimer;
    memset(&cfgTimer, 0, sizeof(cfgTimer));
    cfgTimer.name = "scheduler_sysinfo";
    cfgTimer.callback = schedulerTimerSysInfoExec;
    RE_OK_CHECK_FIRST(logTAG, esp_timer_create(&cfgTimer, &_schedulerTimerSysInfo), return false);
    if (!createSuspened) {
      return schedulerTimerSysInfoStart();
    };
    return true;
  };
  return false;
}

static bool schedulerTimerSysInfoStop()
{
  if (_schedulerTimerSysInfo) {
    if (esp_timer_is_active(_schedulerTimerSysInfo)) {
      if (esp_timer_stop(_schedulerTimerSysInfo) == ESP_OK) {
        rlog_i(logTAG, "Timer [scheduler_sysinfo] was stopped");
      };
    };
    return true;
  };
  return false;
}

static void schedulerTimerSysInfoDelete()
{
  if (_schedulerTimerSysInfo) {
    if (esp_timer_is_active(_schedulerTimerSysInfo)) {
      esp_timer_stop(_schedulerTimerSysInfo);
    };
    RE_OK_CHECK_FIRST(logTAG, esp_timer_delete(_schedulerTimerSysInfo), return);
    _schedulerTimerSysInfo = nullptr;
    rlog_i(logTAG, "Timer [scheduler_sysinfo] was deleted");
  };
}

#endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- TaskList Timer ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if CONFIG_MQTT_TASKLIST_ENABLE

static void schedulerTimerTasksExec(void* arg)
{
  sysinfoPublishTaskList();
}

static bool schedulerTimerTasksStart()
{
  if (_schedulerTimerTasks) {
    if (esp_timer_is_active(_schedulerTimerTasks)) {
      esp_timer_stop(_schedulerTimerTasks);
    };
    RE_OK_CHECK_FIRST(logTAG, esp_timer_start_periodic(_schedulerTimerTasks, CONFIG_MQTT_TASKLIST_INTERVAL * 1000), return false);
    rlog_i(logTAG, "Timer [scheduler_tasks] was started");
    return true;
  };
  return false;
}

static bool schedulerTimerTasksCreate(bool createSuspened)
{
  if (!_schedulerTimerTasks) {
    esp_timer_create_args_t cfgTimer;
    memset(&cfgTimer, 0, sizeof(cfgTimer));
    cfgTimer.name = "scheduler_tasks";
    cfgTimer.callback = schedulerTimerTasksExec;
    RE_OK_CHECK_FIRST(logTAG, esp_timer_create(&cfgTimer, &_schedulerTimerTasks), return false);
    if (!createSuspened) {
      return schedulerTimerTasksStart();
    };
    return true;
  };
  return false;
}

static bool schedulerTimerTasksStop()
{
  if (_schedulerTimerTasks) {
    if (esp_timer_is_active(_schedulerTimerTasks)) {
      if (esp_timer_stop(_schedulerTimerTasks) == ESP_OK) {
        rlog_i(logTAG, "Timer [scheduler_tasks] was stopped");
      };
    };
    return true;
  };
  return false;
}

static void schedulerTimerTasksDelete()
{
  if (_schedulerTimerTasks) {
    if (esp_timer_is_active(_schedulerTimerTasks)) {
      esp_timer_stop(_schedulerTimerTasks);
    };
    RE_OK_CHECK_FIRST(logTAG, esp_timer_delete(_schedulerTimerTasks), return);
    _schedulerTimerTasks = nullptr;
    rlog_i(logTAG, "Timer [scheduler_tasks] was deleted");
  };
}

#endif // CONFIG_MQTT_TASKLIST_ENABLE

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Еvent handlers ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void schedulerEventHandlerTime(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (_schedulerTimerMain) {
    schedulerResume();
  } else {
    schedulerStart(false);
  };
}

static void schedulerOtaEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if ((event_id == RE_SYS_OTA) && (event_data)) {
    re_system_event_data_t* data = (re_system_event_data_t*)event_data;
    if (data->type == RE_SYS_SET) {
      schedulerSuspend();
    } else {
      schedulerResume();
    };
  };
}

#if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE

static void schedulerEventHandlerParams(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_PARAMS_CHANGED)  {
    #if defined(CONFIG_SILENT_MODE_EXTENDED) && CONFIG_SILENT_MODE_EXTENDED
      if ((*(uint32_t*)event_data == (uint32_t)&tsSilentModeTimespan) 
      || (*(uint32_t*)event_data == (uint32_t)&enabledSilentMode)) {
    #else
      if (*(uint32_t*)event_data == (uint32_t)&tsSilentModeTimespan) {
    #endif // CONFIG_SILENT_MODE_EXTENDED
      silentModeCheckExternal();
    };
  };
}

#endif // defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE

bool schedulerEventHandlerRegister()
{
  if (!_handlersRegistered) {
    _handlersRegistered = eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime, nullptr)
            && eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime, nullptr)
            && eventHandlerRegister(RE_SYSTEM_EVENTS, RE_SYS_OTA, &schedulerOtaEventHandler, nullptr);
    #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
      _handlersRegistered = _handlersRegistered && eventHandlerRegister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerParams, nullptr);
    #endif // defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
    #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
      _handlersRegistered = _handlersRegistered && sysinfoEventHandlerRegister();
    #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
  };
  return _handlersRegistered;
}

void schedulerEventHandlerUnregister()
{
  if (_handlersRegistered) {
    _handlersRegistered = false;
    eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime);
    eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime);
    #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
      eventHandlerUnregister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerParams);
    #endif // defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
    #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
      sysinfoEventHandlerUnregister();
    #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Task routines ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

bool schedulerStart(bool createSuspended)
{
  bool ret = false;
  if (!_schedulerTimerMain) {
    ret = schedulerInit() && schedulerTimerMainCreate() && schedulerEventHandlerRegister();
    #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
      silentModeRegister();
    #endif // defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
    #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
      ret = ret && schedulerTimerSysInfoCreate(createSuspended);
    #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    #if CONFIG_MQTT_TASKLIST_ENABLE
      ret = ret && schedulerTimerTasksCreate(createSuspended);
    #endif // CONFIG_MQTT_TASKLIST_ENABLE
  };
  if (!ret) {
    schedulerDelete();
  };
  return ret;
}

bool schedulerSuspend()
{
  bool ret = true;
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    ret = ret && schedulerTimerSysInfoStop();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    #if CONFIG_MQTT_TASKLIST_ENABLE
      ret = ret && schedulerTimerTasksStop();
    #endif // CONFIG_MQTT_TASKLIST_ENABLE
  return ret;
}

bool schedulerResume()
{
  bool ret = true;
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    ret = ret && schedulerTimerSysInfoStart();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    #if CONFIG_MQTT_TASKLIST_ENABLE
      ret = ret && schedulerTimerTasksStart();
    #endif // CONFIG_MQTT_TASKLIST_ENABLE
  return ret;
}

void schedulerDelete()
{
  #if CONFIG_MQTT_TASKLIST_ENABLE
    schedulerTimerTasksDelete();
  #endif // CONFIG_MQTT_TASKLIST_ENABLE
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    schedulerTimerSysInfoDelete();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
  schedulerEventHandlerUnregister();
  schedulerTimerMainDelete();
  schedulerFree();
}

