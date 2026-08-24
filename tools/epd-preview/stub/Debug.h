/* Host stub. The firmware's Debug.h routes to ESP_LOGD, which does not exist
 * here; the vendored sources only use Debug() for trace, so drop it. */
#ifndef _DEBUG_H_
#define _DEBUG_H_
#define Debug(...) do { } while (0)
#endif
