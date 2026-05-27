#ifndef __PROTOBUF_INFRA_H__
#define __PROTOBUF_INFRA_H__

#include <pb.h>
#include <pb_decode.h>
#include <SPIFFS.h>

// reading the file as stream is memory efficient, as no extra buffer is needed,
//      and we don't have to commit for the buffer size in advance.
// however - in a small banchmark I tried, the function call time changed from:
// 3ms - reading the entire file to buffer
// 27ms - reading the buffer as stream.
//
// this makes sense, but should be considered: memory vs. cpu time
// it might not always be the right solution
//
// TODO: can we use stdio buffering to speed things up?
pb_istream_t FileToPbStream(File &f);

pb_istream_t StreamToPbStream(Stream *s, size_t totalSize);

#endif // __PROTOBUF_INFRA_H__
