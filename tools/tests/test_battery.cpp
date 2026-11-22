#include "apps/app_roast/roast_battery.h"
#include <cassert>
#include <cstdio>
int main(){
 assert(roast::BatteryPercent(0)==-1);assert(roast::BatteryPercent(-2)==-1);
 assert(roast::BatteryPercent(2400)==-1);assert(roast::BatteryPercent(5000)==-1);
 assert(roast::BatteryPercent(3000)==0);assert(roast::BatteryPercent(3300)==0);
 assert(roast::BatteryPercent(3700)==50);assert(roast::BatteryPercent(4100)==100);
 assert(roast::BatteryPercent(4200)==100);
 puts("battery: 9 passed");
}
