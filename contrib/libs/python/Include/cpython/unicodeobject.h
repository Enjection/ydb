#pragma once

#ifdef USE_PYTHON3
#include <contrib/tools/python3/Include/cpython/unicodeobject.h>
#else
#error "No <cpython/unicodeobject.h> in Python2"
#endif
