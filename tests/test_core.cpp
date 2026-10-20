#include "../ESP8266_Node/Core.h"
#include <assert.h>
#include <stdio.h>
int main() {
 CardDb d;
 assert(!d.contains("AABBCCDD"));
 assert(d.add("AABBCCDD"));
 assert(d.contains("AABBCCDD"));
 assert(!d.add("AABBCCDD"));
 assert(!d.add("invalid"));
 assert(d.add("01020304050607"));
 assert(d.add("0102030405060708090A"));
 uint8_t b[CardDb::BYTES]; d.encode(b);
 CardDb restored; assert(restored.decode(b, sizeof b));
 assert(restored.contains("AABBCCDD"));
 b[11]^=1; assert(!restored.decode(b,sizeof b));
 assert(restored.count()==0); // Corrupt data fails closed.
 for(int i=3;i<50;i++){char uid[9];snprintf(uid,sizeof uid,"%08X",i);assert(d.add(uid));}
 assert(!d.add("FFFFFFFF"));
 Window w; assert(!w.active);w.start(0xFFFFFFF0u);
 assert(!w.expired(0xFFFFFFF0u+19999u));assert(w.expired(0xFFFFFFF0u+20000u));
 w.cancel(); assert(!w.active);
 LineBuffer<8> line; assert(!line.push('A'));assert(line.push('\n'));assert(!strcmp(line.text(),"A"));
 for(int i=0;i<10;i++)assert(!line.push('x'));
 assert(!line.push('\n'));assert(!line.push('O'));assert(!line.push('K'));assert(line.push('\n'));
 assert(!strcmp(line.text(),"OK"));
 RisingEdge edge; assert(!edge.sample(false));assert(edge.sample(true));assert(!edge.sample(true));assert(!edge.sample(false));assert(edge.sample(true));
 puts("PASS: UID matching, duplicates, validation, capacity, persistence round-trip/corruption, timer wrap, line overflow, motion edges");
}
