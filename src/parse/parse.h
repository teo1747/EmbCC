#ifndef EMBCC_PARSE_PARSE_H
#define EMBCC_PARSE_PARSE_H

#include "ast.h"

struct unit *parse_unit(const char *file, const char *src);

#endif
