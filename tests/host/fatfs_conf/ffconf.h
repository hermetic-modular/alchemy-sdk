/* Host test build: reuse the exact FatFS configuration the target
 * firmware compiles against, without exposing the rest of libDaisy's
 * src/sys headers to the stub-backed test build. */
#include "../../../vendor/libDaisy/src/sys/ffconf.h"
