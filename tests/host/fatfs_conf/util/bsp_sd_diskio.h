/* Host test build: the target ffconf.h pulls the BSP SD glue via ST's
 * "additional user header" hook; nothing in the FatFS core actually
 * needs it, and the RAM diskio replaces the BSP entirely — stub it. */
#pragma once
