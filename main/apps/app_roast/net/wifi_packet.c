// SPDX-License-Identifier: MIT
#include "wifi_packet.h"
#include <string.h>
static bool utf8(const uint8_t *s,size_t n) {
    for(size_t i=0;i<n;) {
        uint32_t c=s[i++];unsigned extra=0;
        if(c==0)return false;
        if(c<128)continue;
        uint32_t minimum;
        if(c>=0xc2&&c<=0xdf) {extra=1;minimum=128;c&=31;}
        else if(c>=0xe0&&c<=0xef) {extra=2;minimum=2048;c&=15;}
        else if(c>=0xf0&&c<=0xf4) {extra=3;minimum=65536;c&=7;}
        else return false;
        if(i+extra>n)return false;
        while(extra--) {if((s[i]&0xc0)!=0x80)return false;c=(c<<6)|(s[i++]&63);}
        if(c<minimum||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
    }
    return true;
}
bool cw_wifi_packet_decode(const uint8_t *d,size_t n,cw_wifi_packet_t *out) {
    if(!d||!out||n<8||d[0]!=1)return false;
    unsigned ssid=d[2],password=d[3];
    if(n!=8u+ssid+password||ssid>32||password>64)return false;
    if(d[1]==2) {if(ssid||password)return false;}
    else if(d[1]==1) {
        if(!ssid||!utf8(d+8,ssid))return false;
        if(password&&password<8)return false;
        for(unsigned i=0;i<password;i++) {
            uint8_t c=d[8+ssid+i];if(!c)return false;
            if(password==64&&!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;
        }
    } else return false;
    uint32_t request=(uint32_t)d[4]|(uint32_t)d[5]<<8|(uint32_t)d[6]<<16|(uint32_t)d[7]<<24;
    if(!request)return false;
    memset(out,0,sizeof *out);out->op=d[1];out->request_id=request;
    memcpy(out->ssid,d+8,ssid);memcpy(out->password,d+8+ssid,password);return true;
}
