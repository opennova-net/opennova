#ifndef THREEDI_MODEL_H
#define THREEDI_MODEL_H

#include "threedi/threedi.h"
#include "threedi/threedi_3di3.h"

typedef Threedi3di3 ThreediModel;

#define THREEDI_DEFAULT_VERSION 259u
#define threedi_model_write threedi_3di3_write
#define threedi_model_free  threedi_3di3_free

#endif
