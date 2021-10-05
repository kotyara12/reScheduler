/* 
   EN: Library for working with schedules
   RU: Библиотека для работы с расписаниями
   --------------------------
   (с) 2021 Разживин Александр | Razzhivin Alexander
   kotyara12@yandex.ru | https://kotyara12.ru | tg: @kotyara1971
*/

#ifndef __RE_SCHEDULER_H__
#define __RE_SCHEDULER_H__

#include <stdbool.h>
#include "rTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

bool schedulerInit();
void schedulerFree();
void schedulerRegister(timespan_t* timespan, uint32_t value);

bool schedulerTaskCreate(bool createSuspended);
bool schedulerTaskSuspend();
bool schedulerTaskResume();
void schedulerTaskDelete();

bool schedulerEventHandlerRegister();
void schedulerEventHandlerUnregister();

// Silent mode
#if CONFIG_SILENT_MODE_ENABLE
bool isSilentMode();
#endif // CONFIG_SILENT_MODE_ENABLE

#ifdef __cplusplus
}
#endif

#endif // __RE_SCHEDULER_H__