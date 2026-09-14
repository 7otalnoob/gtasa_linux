#ifndef GTASA_AML_MOD_H
#define GTASA_AML_MOD_H

#include "so_util.h"

/* Load opt-in AML-style modules from mods/ after the game image is ready. */
void aml_load_mods(const char *directory, so_module *game);

#endif
