#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

inline bool elapsed(uint32_t now, uint32_t since, uint32_t duration) {
  return uint32_t(now - since) >= duration;
}
struct Window {
  bool active = false;
  uint32_t since = 0;
  void start(uint32_t now) { active = true; since = now; }
  void cancel() { active = false; }
  bool expired(uint32_t now) const { return active && elapsed(now, since, 20000); }
};
struct RisingEdge {
  bool previous = false;
  bool sample(bool level) { bool rising = level && !previous; previous = level; return rising; }
};
template<size_t N> class LineBuffer {
  char buffer[N] = {};
  size_t used = 0;
  bool dropping = false;
public:
  void reset() { used = 0; dropping = false; buffer[0] = 0; }
  bool push(char c) {
    if(c == '\r') return false;
    if(c == '\n') {
      if(dropping) { reset(); return false; }
      buffer[used] = 0; used = 0; return true;
    }
    if(dropping) return false;
    if(used >= N-1) { dropping = true; return false; }
    buffer[used++] = c; return false;
  }
  const char* text() const { return buffer; }
};
class CardDb {
  uint8_t n = 0;
  char cards[50][21] = {};
  static uint32_t crc(const uint8_t* p, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    while(len--) {
      c ^= *p++;
      for(int i=0;i<8;i++) c=(c>>1) ^ ((c&1) ? 0xEDB88320u : 0);
    }
    return ~c;
  }
public:
  enum { BYTES = 4+1+50*21+4 };
  uint8_t count() const { return n; }
  static bool valid(const char* uid) {
    size_t len = strlen(uid);
    if(len!=8 && len!=14 && len!=20) return false;
    for(size_t i=0;i<len;i++) if(!((uid[i]>='0' && uid[i]<='9') || (uid[i]>='A' && uid[i]<='F'))) return false;
    return true;
  }
  bool contains(const char* uid) const {
    for(uint8_t i=0;i<n;i++) if(!strcmp(cards[i],uid)) return true;
    return false;
  }
  bool add(const char* uid) {
    if(!valid(uid) || contains(uid) || n>=50) return false;
    strcpy(cards[n++],uid); return true;
  }
  void encode(uint8_t* b) const {
    memcpy(b,"UID1",4); b[4]=n; memcpy(b+5,cards,sizeof cards);
    uint32_t c=crc(b,BYTES-4);
    for(int i=0;i<4;i++) b[BYTES-4+i]=uint8_t(c>>(8*i));
  }
  bool decode(const uint8_t* b, size_t len) {
    n=0; memset(cards,0,sizeof cards);
    if(len!=BYTES || memcmp(b,"UID1",4) || b[4]>50) return false;
    uint32_t c=0;
    for(int i=0;i<4;i++) c |= uint32_t(b[BYTES-4+i])<<(8*i);
    if(c!=crc(b,BYTES-4)) return false;
    for(uint8_t i=0;i<b[4];i++) {
      const char* s=reinterpret_cast<const char*>(b+5+21*i);
      if(!memchr(s,0,21) || !valid(s)) return false;
      for(uint8_t j=0;j<i;j++) if(!strcmp(s,reinterpret_cast<const char*>(b+5+21*j))) return false;
    }
    n=b[4]; memcpy(cards,b+5,sizeof cards); return true;
  }
};
