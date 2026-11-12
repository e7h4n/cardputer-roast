// SPDX-License-Identifier: MIT
#include "dropbox_packet.h"
#include <string.h>
static bool alnum(uint8_t c) {return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');}
bool cw_dropbox_packet_decode(const uint8_t *d,size_t n,cw_dropbox_command_t *out) {
    if(!d||!out||n<9||d[0]!=2)return false;
    size_t a=d[2],b=d[3],c=d[8];
    if(n!=9+a+b+c||a>CW_DROPBOX_KEY_MAX||b>CW_DROPBOX_SECRET_MAX||c>CW_DROPBOX_CODE_MAX)return false;
    if(d[1]==1) {
        if(!a||!b||!c)return false;
        // App key and secret are letters and digits; an authorization code may also use - and _.
        for(size_t i=9;i<9+a+b;i++)if(!alnum(d[i]))return false;
        for(size_t i=9+a+b;i<n;i++)if(!alnum(d[i])&&d[i]!='-'&&d[i]!='_')return false;
    } else if((d[1]!=2&&d[1]!=3)||a||b||c)return false;
    uint32_t id=(uint32_t)d[4]|(uint32_t)d[5]<<8|(uint32_t)d[6]<<16|(uint32_t)d[7]<<24;
    if(!id)return false;
    memset(out,0,sizeof *out);out->op=d[1];out->request=id;
    memcpy(out->key,d+9,a);memcpy(out->secret,d+9+a,b);memcpy(out->code,d+9+a+b,c);return true;
}
