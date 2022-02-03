#include <time.h>
#include "reScheduler.h"
#include "reEvents.h"
#include "rLog.h"
#include "rTypes.h"
#include "reNvs.h"
#include "reEsp32.h"
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
static const char* schedulerTaskName = "scheduler";

TaskHandle_t _schedulerTask;

#if CONFIG_SCHEDULER_STATIC_ALLOCATION
StaticTask_t _schedulerTaskBuffer;
StackType_t _schedulerTaskStack[CONFIG_SCHEDULER_STACK_SIZE];
#endif // CONFIG_SCHEDULER_STATIC_ALLOCATION

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

static timespan_t tsSilentMode = CONFIG_SILENT_MODE_INTERVAL;
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
    bool newSilentMode = checkTimespan(timeinfo, tsSilentMode);
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
  #if CONFIG_MQTT_TASKLIST_ENABLE
  static esp_timer_t timerTaskList;
  timerSet(&timerTaskList, CONFIG_MQTT_TASKLIST_INTERVAL);
  #endif // CONFIG_MQTT_TASKLIST_ENABLE

  while (true) {
    // Get the current time
    nowT = time(nullptr);
    localtime_r(&nowT, &nowS);
    if (nowS.tm_min != minLast) {
      minLast = nowS.tm_min;

      // Publish an event every minute
      eventLoopPost(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, &nowS.tm_min, sizeof(int), portMAX_DELAY);

      // Publish an event about beginning of next interval
      if (nowS.tm_min == 0) {
        eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_HOUR, &nowS.tm_hour, sizeof(int), portMAX_DELAY);
        if (nowS.tm_hour == 0) {
          eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_DAY, &nowS.tm_mday, sizeof(int), portMAX_DELAY);
          if (nowS.tm_wday == CONFIG_FORMAT_FIRST_DAY_OF_WEEK) {
            eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_WEEK, &nowS.tm_wday, sizeof(int), portMAX_DELAY);
          };
          if (nowS.tm_mday == 1) {
            eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_MONTH, &nowS.tm_mon, sizeof(int), portMAX_DELAY);
            if (nowS.tm_mon == 1) {
              eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_YEAR, &nowS.tm_year, sizeof(int), portMAX_DELAY);
            };
          };
        };
      };

      // Calculate the operating time of the device
      sysinfoWorkTimeInc();

      if (nowT > 1000000000) {
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
    };

    // Publish system information
    #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE || CONFIG_EVENT_LOOP_STATISTIC_ENABLED
    if (timerTimeout(&timerSysInfo)) {
      timerSet(&timerSysInfo, CONFIG_MQTT_SYSINFO_INTERVAL);
      
      #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
      sysinfoPublishSysInfo();
      #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    };
    #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE || CONFIG_EVENT_LOOP_STATISTIC_ENABLED

    #if CONFIG_MQTT_TASKLIST_ENABLE
    if (timerTimeout(&timerTaskList)) {
      timerSet(&timerTaskList, CONFIG_MQTT_TASKLIST_INTERVAL);
      sysinfoPublishTaskList();
    };
    #endif // CONFIG_MQTT_TASKLIST_ENABLE

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
      schedulerInit();
      #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
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
    schedulerFree();
    rloga_d("Task [ %s ] was deleted", schedulerTaskName);
  };
}

// -----------------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------- Еvent handlers ---------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

static void schedulerEventHandlerTime(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (_schedulerTask) {
    schedulerTaskResume();
  } else {
    schedulerTaskCreate(false);
  };
}

static void schedulerOtaEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if ((event_id == RE_SYS_OTA) && (event_data)) {
    re_system_event_data_t* data = (re_system_event_data_t*)event_data;
    if (data->type == RE_SYS_SET) {
      schedulerTaskSuspend();
    } else {
      schedulerTaskResume();
    };
  };
}

#if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE

static void schedulerEventHandlerParams(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_PARAMS_CHANGED)  {
    if (*(uint32_t*)event_data == (uint32_t)&tsSilentMode) {
      silentModeCheckExternal();
    };
  };
}

#endif // defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE

bool schedulerEventHandlerRegister()
{
  bool ret = eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime, nullptr)
          && eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime, nullptr)
          && eventHandlerRegister(RE_SYSTEM_EVENTS, RE_SYS_OTA, &schedulerOtaEventHandler, nullptr);
  #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
    ret = ret && eventHandlerRegister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerParams, nullptr);
  #endif // defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    ret = ret && sysinfoEventHandlerRegister();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
  return ret;
}

void schedulerEventHandlerUnregister()
{
  eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime);
  eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime);
  #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
    eventHandlerUnregister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerParams);
  #endif // defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    sysinfoEventHandlerUnregister();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
}
