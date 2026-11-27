#pragma once
#include "coop_net.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DISH_IDLE, DISH_QUEUED, DISH_SENDING, DISH_SENT, DISH_RECEIVED,
    DISH_RETURNING, DISH_RETURNED, DISH_TOO_FAR, DISH_CHANGED,
};

bool coop_dish_enabled(void);
bool coop_dish_can_give(void);
int coop_dish_list(CoopDish out[COOP_DISH_MAX]);
bool coop_dish_request(const CoopDish *dish);
int coop_dish_status(bool recent_only);

#ifdef __cplusplus
}
#endif
