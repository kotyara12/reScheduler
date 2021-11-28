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

#if (defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE) || (defined(CONFIG_MULTI_TARIFF_ENABLE) && CONFIG_MULTI_TARIFF_ENABLE)
#define __SCHEDULER_REGISTER_PARAMS__ 1
#else
#define __SCHEDULER_REGISTER_PARAMS__ 1
#endif

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
    schedulerItems = (schedulerItemHeadHandle_t)calloc(1, sizeof(schedulerItemHead_t));
    if (schedulerItems) {
      STAILQ_INIT(schedulerItems);
    }
    else {
      rlog_e(logTAG, "Scheduler initialization error!");
      return false;
    }
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

void schedulerRegister(timespan_t* timespan, uint32_t value)
{
  if (schedulerItems) {
    schedulerItemHandle_t item = (schedulerItemHandle_t)calloc(1, sizeof(schedulerItem_t));
    if (item) {
      item->timespan = timespan;
      item->value = value;
      item->state = -1;
      STAILQ_INSERT_TAIL(schedulerItems, item, next);
    };
  };
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
// ----------------------------------------------------- Multi tariff ----------------------------------------------------
// -----------------------------------------------------------------------------------------------------------------------

#if defined(CONFIG_MULTI_TARIFF_ENABLE) && CONFIG_MULTI_TARIFF_ENABLE

static uint8_t iTariff = 0;
static const char* tagMT = "MTRF";

// Tariff NIGHT
#ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1
static timespan_t tsTariffNight1 = CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1;
#endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1
#ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
static timespan_t tsTariffNight2 = CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2;
#endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
#ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3
static timespan_t tsTariffNight3 = CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3;
#endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3

// Tariff SELF
#ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
static timespan_t tsTariffSelf1 = CONFIG_MULTI_TARIFF_INTERVAL_SELF_1;
#endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
#ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
static timespan_t tsTariffSelf2 = CONFIG_MULTI_TARIFF_INTERVAL_SELF_2;
#endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
#ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_3
static timespan_t tsTariffSelf3 = CONFIG_MULTI_TARIFF_INTERVAL_SELF_3;
#endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_3

void multiTariffRegister()
{
  paramsGroupHandle_t pgTariffs = paramsRegisterGroup(nullptr, 
    CONFIG_MULTI_TARIFF_GROUP_KEY, CONFIG_MULTI_TARIFF_GROUP_TOPIC, CONFIG_MULTI_TARIFF_GROUP_NAME);

  // Tariff NIGHT
  #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1
    paramsGroupHandle_t pgTariffNight = paramsRegisterGroup(pgTariffs, 
      CONFIG_MULTI_TARIFF_NIGHT_KEY, CONFIG_MULTI_TARIFF_NIGHT_TOPIC, CONFIG_MULTI_TARIFF_NIGHT_NAME);
    #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffNight,
        CONFIG_MULTI_TARIFF_TIMESPAN_1_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_1_NAME,
        CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffNight1);
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffNight,
        CONFIG_MULTI_TARIFF_TIMESPAN_2_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_2_NAME,
        CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffNight2);
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3
        paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffNight,
          CONFIG_MULTI_TARIFF_TIMESPAN_3_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_3_NAME,
          CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffNight3);
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3
    #else
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffNight,
        CONFIG_MULTI_TARIFF_TIMESPAN_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_NAME,
        CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffNight1);
    #endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
  #endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1

  // Tariff SELF
  #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
    paramsGroupHandle_t pgTariffSelf = paramsRegisterGroup(pgTariffs, 
      CONFIG_MULTI_TARIFF_SELF_KEY, CONFIG_MULTI_TARIFF_SELF_TOPIC, CONFIG_MULTI_TARIFF_SELF_NAME);
    #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffSelf,
        CONFIG_MULTI_TARIFF_TIMESPAN_1_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_1_NAME,
        CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffSelf1);
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffSelf,
        CONFIG_MULTI_TARIFF_TIMESPAN_2_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_2_NAME,
        CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffSelf2);
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_3
        paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffSelf,
          CONFIG_MULTI_TARIFF_TIMESPAN_3_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_3_NAME,
          CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffSelf3);
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_3
    #else
      paramsRegisterValue(OPT_KIND_PARAMETER, OPT_TYPE_TIMESPAN, nullptr, pgTariffSelf,
        CONFIG_MULTI_TARIFF_TIMESPAN_TOPIC, CONFIG_MULTI_TARIFF_TIMESPAN_NAME,
        CONFIG_MQTT_PARAMS_QOS, (void*)&tsTariffSelf1);
    #endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
  #endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
}

void multiTariffCheck(const struct tm timeinfo)
{
  // Tariff NIGHT
  bool isT3 = false;
  #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1
    isT3 = isT3 || ((tsTariffNight1 > 0) && checkTimespan(timeinfo, tsTariffNight1));
    #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
      isT3 isT3 || ((tsTariffNight2 > 0) && checkTimespan(timeinfo, tsTariffNight2));
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3
        isT3 isT3 || ((tsTariffNight3 > 0) && checkTimespan(timeinfo, tsTariffNight3));
      #endif //CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3
    #endif //CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
  #endif //CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1
  
  // Tariff SELF
  bool isT2 = false;
  #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
    isT2 = isT2 || ((tsTariffSelf1 > 0) && checkTimespan(timeinfo, tsTariffSelf1));
    #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
      isT2 = isT2 || ((tsTariffSelf2 > 0) && checkTimespan(timeinfo, tsTariffSelf2));
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_3
        isT2 = isT2 || ((tsTariffSelf3 > 0) && checkTimespan(timeinfo, tsTariffSelf3));
      #endif //CONFIG_MULTI_TARIFF_INTERVAL_SELF_3
    #endif //CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
  #endif //CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
  
  uint8_t newTariff = 1;
  if (isT3) {
    newTariff = 3;
  } else if (isT2) {
    newTariff = 2;
  };

  // If the tariff has changed
  if (iTariff != newTariff) {
    iTariff = newTariff;
    rlog_i(tagMT, "Tariff %d activated", newTariff);
    eventLoopPost(RE_TIME_EVENTS, RE_TIME_TARIFF_CHANGED, &newTariff, sizeof(newTariff), portMAX_DELAY);
  };
}

void multiTariffCheckExternal()
{
  time_t nowT;
  struct tm nowS;
  nowT = time(nullptr);
  localtime_r(&nowT, &nowS);
  multiTariffCheck(nowS);
}

uint8_t multiTariffGetTariff()
{
  return iTariff;
}

#endif // CONFIG_MULTI_TARIFF_ENABLE

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
      eventLoopPost(RE_TIME_EVENTS, RE_TIME_EVERY_MINUTE, nullptr, 0, portMAX_DELAY);

      // Publish an event about beginning of next interval
      if (nowS.tm_min == 0) {
        eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_HOUR, nullptr, 0, portMAX_DELAY);
        if (nowS.tm_hour == 0) {
          eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_DAY, nullptr, 0, portMAX_DELAY);
          if (nowS.tm_wday == CONFIG_FORMAT_FIRST_DAY_OF_WEEK) {
            eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_WEEK, nullptr, 0, portMAX_DELAY);
          };
          if (nowS.tm_mday == 1) {
            eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_MONTH, nullptr, 0, portMAX_DELAY);
            if (nowS.tm_mon == 1) {
              eventLoopPost(RE_TIME_EVENTS, RE_TIME_START_OF_YEAR, nullptr, 0, portMAX_DELAY);
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

        #if defined(CONFIG_MULTI_TARIFF_ENABLE) && CONFIG_MULTI_TARIFF_ENABLE
        multiTariffCheck(nowS);
        #endif // CONFIG_MULTI_TARIFF_ENABLE
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
      #if defined(CONFIG_MULTI_TARIFF_ENABLE) && CONFIG_MULTI_TARIFF_ENABLE
      multiTariffRegister();
      #endif // CONFIG_MULTI_TARIFF_ENABLE
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

#if __SCHEDULER_REGISTER_PARAMS__

static void schedulerEventHandlerParams(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
  if (event_id == RE_PARAMS_CHANGED)  {
    #if defined(CONFIG_SILENT_MODE_ENABLE) && CONFIG_SILENT_MODE_ENABLE
      if (*(uint32_t*)event_data == (uint32_t)&tsSilentMode) {
        silentModeCheckExternal();
      };
    #endif // CONFIG_SILENT_MODE_ENABLE

    #if defined(CONFIG_MULTI_TARIFF_ENABLE) && CONFIG_MULTI_TARIFF_ENABLE
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1
        if (*(uint32_t*)event_data == (uint32_t)&tsTariffNight1) {
          multiTariffCheckExternal();
        };
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_1
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
        if (*(uint32_t*)event_data == (uint32_t)&tsTariffNight2) {
          multiTariffCheckExternal();
        };
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_2
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3
        if (*(uint32_t*)event_data == (uint32_t)&tsTariffNight3) {
          multiTariffCheckExternal();
        };
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_NIGHT_3

      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
        if (*(uint32_t*)event_data == (uint32_t)&tsTariffSelf1) {
          multiTariffCheckExternal();
        };
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_1
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
        if (*(uint32_t*)event_data == (uint32_t)&tsTariffSelf2) {
          multiTariffCheckExternal();
        };
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_2
      #ifdef CONFIG_MULTI_TARIFF_INTERVAL_SELF_3
        if (*(uint32_t*)event_data == (uint32_t)&tsTariffSelf3) {
          multiTariffCheckExternal();
        };
      #endif // CONFIG_MULTI_TARIFF_INTERVAL_SELF_3
    #endif // CONFIG_MULTI_TARIFF_ENABLE
  };
}

#endif // __SCHEDULER_REGISTER_PARAMS__

bool schedulerEventHandlerRegister()
{
  bool ret = eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime, nullptr)
          && eventHandlerRegister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime, nullptr);
  #if __SCHEDULER_REGISTER_PARAMS__
    ret = ret && eventHandlerRegister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerParams, nullptr);
  #endif // __SCHEDULER_REGISTER_PARAMS__
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    ret = ret && sysinfoEventHandlerRegister();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
  return ret;
}

void schedulerEventHandlerUnregister()
{
  eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_RTC_ENABLED, &schedulerEventHandlerTime);
  eventHandlerUnregister(RE_TIME_EVENTS, RE_TIME_SNTP_SYNC_OK, &schedulerEventHandlerTime);
  #if __SCHEDULER_REGISTER_PARAMS__
    eventHandlerUnregister(RE_PARAMS_EVENTS, RE_PARAMS_CHANGED, &schedulerEventHandlerParams);
  #endif // __SCHEDULER_REGISTER_PARAMS__
  #if CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
    sysinfoEventHandlerUnregister();
  #endif // CONFIG_MQTT_STATUS_ONLINE || CONFIG_MQTT_SYSINFO_ENABLE
}
