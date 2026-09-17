#ifndef GTASA_AML_IFACE_H
#define GTASA_AML_IFACE_H

// Minimal IAML-compatible interface, standing in for the real libAML.so
// that AML mods (AndroidModLoader ecosystem) expect to dlopen() at load
// time. See aml_iface.c for the full explanation.
//
// name: interface name the mod asked for (mod/amlmod.h's MYMOD() macro
// always asks for "AMLInterface"). Returns a pointer usable as an IAML* on
// the caller's side, or NULL if the name isn't recognized -- matching the
// real GetInterface(const char*) ABI exactly, so a mod compiled against
// the real AML SDK works against this without any changes.
void *aml_get_interface(const char *name);

#endif
