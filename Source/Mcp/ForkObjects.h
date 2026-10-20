/*
 // The fork's own GUI objects (Plan 3 P3.2 S8): [particlepad], [ai.midi], [ai.voice].
 // ObjectBase::createGui asks here first (one hook), so the upstream factory switch
 // stays as it was.
 */
#pragma once

#include "Utility/Hash.h"
#include "Pd/WeakReference.h"

class ObjectBase;
class Object;

// The GUI for a fork object class, or nullptr when `name` is not one of them.
ObjectBase* createForkObjectGui(hash32 name, pd::WeakReference ptr, Object* parent);
